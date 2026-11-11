#!/usr/bin/env python3
"""Writes the synthetic Steam Workshop replies that tests/workshop_test.c reads.

  python3 -I tools/dev/genworkshopfix.py <source tree>/tests/fixtures/workshop

Nothing is fetched and nothing here describes a real item: the ids are in a range the
Workshop does not use (9990000001 and up), the creators are numbers below the smallest
SteamID64 of an account (76561197960265728), titles, names and descriptions are invented
and every URL points to example.org. Only the SHAPE is Steam's: the members and their
types as the Workshop's pages and Web API replies carry them, so that the parsers in
src/common/workshop_query.c meet what they meet in real replies:

  browse_trend_p1.html               a browse page: server-rendered JSON in a script tag
                                     (a JSON string inside JSON), 30 results of 1234567,
                                     a navigation link in front that is no result
  popular_excl.html                  the same with required / excluded tags honoured
  search_cyberpunk_mostrecent.html   a search page
  search_rare.html                   a search with three results on one page
  legacy_links.html                  the older markup: item links, one of them twice
  details_trend_p1.json              GetPublishedFileDetails for the 30 ids of the first page
  details_one_missing.json           ... for one item that exists and one that does not
  queryfiles_synthetic.json          IPublishedFileService/QueryFiles, five items
  (error_page.html is written by hand and stays as it is.)

The output is the same on every run (no clock, no random module). workshop_test.c checks
values written here: change both together.
"""
import json, os, sys

APP = 431960
ID0 = 9990000001            # not a range the Workshop hands out
CREATOR0 = 76561190000000001  # below the first account's SteamID64
T0 = 1700000000             # a fixed "now"

ADJ = ["Quiet", "Paper", "Amber", "Northern", "Hollow", "Silver", "Late", "Open", "Winter", "Glass",
       "Distant", "Painted", "Slow", "Copper", "Morning"]
NOUN = ["Harbour", "Lanterns", "Orchard", "Signal", "Rooftops", "Meadow", "Station", "Tide", "Atrium",
        "Foothills", "Crossing", "Workshop", "Balcony", "Causeway", "Greenhouse"]
# titles that are awkward on purpose: markup characters, quotes, scripts other than Latin,
# a combining mark, an emoji, text that runs right to left, leading / trailing blanks
ODD = ["Tide & Lanterns", "<b>Not bold</b> \"quoted\"", "雨の駅 - Rainy Station", "Café ́ au lait",
       "Night \U0001F303 drive", "שלום Harbour", "  padded title  ", "100% [b]plain[/b]"]
KINDS = ["Scene", "Video", "Web"]
GENRES = ["Anime", "Landscape", "Abstract", "Pixel art", "Nature", "Sci-Fi", "Relaxing"]
RES = ["1920 x 1080", "2560 x 1440", "3840 x 2160", "Other resolution"]
AGE = ["Everyone", "Questionable", "Mature"]


class Lcg:
    """a small fixed generator: the same numbers on every Python"""
    def __init__(self, seed):
        self.s = seed & 0xffffffff

    def next(self, n):
        self.s = (self.s * 1664525 + 1013904223) & 0xffffffff
        return (self.s >> 8) % n


def title(i):
    return ODD[i] if i < len(ODD) else "%s %s %d" % (ADJ[i % len(ADJ)], NOUN[(i * 7 + 3) % len(NOUN)], i + 1)


def tags(i, r, age=None, genre=None, kind=None):
    names = [kind or KINDS[r.next(3)], genre or GENRES[r.next(len(GENRES))], RES[r.next(len(RES))],
             age or AGE[0 if r.next(5) else 1 + r.next(2)], "Approved"]
    if i % 6 == 0:
        names.append("Media Integration")
    return [{"tag": n, "display_name": n} for n in names]


def describe(i):
    return ("[h1]%s[/h1]\r\nAn invented item for WalPE's tests, number %d.\n[list][*]made by a script"
            "[*]describes nothing real[/list]\n[url=https://example.org/walpe-test/%d]example.org[/url]"
            % (title(i).strip(), i + 1, i + 1))


def size(i, r):
    # sizes from a few hundred KiB to beyond 4 GiB (64 bits, and as a string, as Steam sends them)
    if i == 0:
        return "4831838208"
    return str(350000 + r.next(1 << 20) * (1 + r.next(900)))


def browse_item(i, r, **tag_args):
    return {
        "publishedfileid": str(ID0 + i), "creator": str(CREATOR0 + i % 11), "consumer_appid": APP,
        "file_type": 0, "preview_url": "https://example.org/walpe-test/preview/%d.jpg" % (ID0 + i),
        "title": title(i), "short_description": "Invented item %d." % (i + 1), "workshop_accepted": False,
        "flags": 0, "reactions": [], "num_children": 0, "children": [], "previews": [],
        "time_created": T0 - 86400 * (40 + i), "time_updated": T0 - 3600 * (1 + i), "file_size": size(i, r),
        "tags": tags(i, r, **(dict(kind="Scene") if i == 0 and not tag_args else tag_args)),
        "content_descriptorids": [],
        "subscriptions": 1000 + r.next(50000), "favorited": 10 + r.next(900),
        "lifetime_subscriptions": 2000 + r.next(90000), "lifetime_favorited": 20 + r.next(1500),
        "lifetime_playtime_sessions": "0", "views": 500 + r.next(200000), "num_comments_public": r.next(40),
        "hcontent_file": str(1000000000000000000 + i), "consumer_shortcutid": 0,
        "star_rating": 4 if i == 0 else 3 + r.next(3), "total_votes": 83 if i == 0 else 5 + r.next(4000),
        "link_tags": [],
    }


def creators(items):
    seen, out = set(), []
    for it in items:
        c = it["creator"]
        if c in seen:
            continue
        seen.add(c)
        n = int(c) - CREATOR0 + 1
        out.append({"public_data": {"steamid": c, "visibility_state": 3, "profile_state": 1,
                                    "sha_digest_avatar": {"_t": "bytes", "v": ""},
                                    "persona_name": "example_author_%d" % n,
                                    "profile_url": "example_author_%d" % n,
                                    "content_country_restricted": False},
                    "private_data": {"time_created": T0 - 86400 * 365 * (1 + n % 9)}})
    return out


def page(items, total, pages, key):
    """the page as the Workshop serves it: the query results as a JSON string inside the JSON of
    a script tag; a link in the navigation that looks like an item link in front of it"""
    q = {"admin_view": False, "appid": APP, "browse_sort": "trend", "childpublishedfileid": "",
         "num_per_page": 30, "page": 1, "required_apps_preset": 0, "search_text": "",
         "search_text_target": 0, "section": "readytouseitems", "trend_days": 7}
    q.update(key)
    data = {"eresult": 1, "current_page": 1, "total_pages": pages, "total_count": total, "results": items,
            "next_cursor": "*", "creator_player_link_details": creators(items)}
    queries = {"mutations": [], "queries": [
        {"state": {"data": None}, "queryKey": ["app_admin_access", APP]},
        {"state": {"data": data}, "queryKey": ["workshop_browse", q, 1]}]}
    root = {"route": "/app/%d/workshop/" % APP,
            "renderContext": {"localizationSettings": {}, "queryData": json.dumps(queries, ensure_ascii=False)},
            "loaderData": []}
    # "</" must not appear inside a script element
    body = json.dumps(root, ensure_ascii=False).replace("</", "<\\/")
    return ("<!DOCTYPE html><html><head><title>Workshop test page</title></head><body><div class=\"x\">"
            "<a href=\"https://example.org/sharedfiles/filedetails/?id=1\">nav</a></div>\n"
            "<script type=\"application/json\" id=\"valve-ssr-data\" nonce=\"x\">" + body + "</script>\n"
            "</body></html>\n")


def detail_item(i, r, described=True):
    it = {
        "publishedfileid": str(ID0 + i), "result": 1, "creator": str(CREATOR0 + i % 11),
        "creator_app_id": APP, "consumer_app_id": APP, "filename": "", "file_size": size(i, r), "file_url": "",
        "hcontent_file": str(1000000000000000000 + i),
        "preview_url": "https://example.org/walpe-test/preview/%d.jpg" % (ID0 + i),
        "hcontent_preview": str(2000000000000000000 + i), "title": title(i),
        "description": describe(i), "time_created": T0 - 86400 * (40 + i), "time_updated": T0 - 3600 * (1 + i),
        "visibility": 0, "banned": 0, "ban_reason": "", "subscriptions": 1000 + r.next(50000),
        "favorited": 10 + r.next(900), "lifetime_subscriptions": 2000 + r.next(90000),
        "lifetime_favorited": 20 + r.next(1500), "views": 500 + r.next(200000), "tags": tags(i, r),
    }
    if not described:
        del it["description"]
    return it


def write(out, name, text):
    with open(os.path.join(out, name), "w", encoding="utf-8", newline="") as f:
        f.write(text)
    print("%-36s %6d bytes" % (name, len(text.encode("utf-8"))))


def main():
    if len(sys.argv) != 2:
        sys.stderr.write(__doc__)
        return 2
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)

    r = Lcg(1)
    write(out, "browse_trend_p1.html",
          page([browse_item(i, r) for i in range(30)], 1234567, 1000, {}))
    r = Lcg(2)
    write(out, "popular_excl.html",
          page([browse_item(100 + i, r, genre="Anime", age="Everyone") for i in range(30)], 654321, 1000,
               {"browse_sort": "totaluniquesubscribers", "required_tags": ["Anime"],
                "excluded_tags": ["Mature", "Questionable"]}))
    r = Lcg(3)
    write(out, "search_cyberpunk_mostrecent.html",
          page([browse_item(200 + i, r) for i in range(30)], 4321, 145,
               {"browse_sort": "mostrecent", "search_text": "cyberpunk"}))
    r = Lcg(4)
    write(out, "search_rare.html",
          page([browse_item(300 + i, r) for i in range(3)], 3, 1,
               {"browse_sort": "textsearch", "search_text": "an unlikely phrase"}))

    a, b = ID0 + 400, ID0 + 401
    write(out, "legacy_links.html",
          "<html><body><div class=\"workshopItem\">"
          "<a href=\"https://example.org/sharedfiles/filedetails/?id=%d&searchtext=\" "
          "data-publishedfileid=\"%d\">x</a>"
          "<a href=\"https://example.org/sharedfiles/filedetails/?id=%d\">dup</a>"
          "<a href=\"https://example.org/sharedfiles/filedetails/?id=%d\">y</a></div></body></html>\n"
          % (a, a, a, b))

    r = Lcg(5)
    write(out, "details_trend_p1.json", json.dumps(
        {"response": {"result": 1, "resultcount": 30,
                      "publishedfiledetails": [detail_item(i, r) for i in range(30)]}}, ensure_ascii=False) + "\n")

    r = Lcg(6)
    one = detail_item(500, r)
    one["file_size"] = "27262976"
    one["subscriptions"] = 345678
    one["tags"] = [{"tag": n, "display_name": n} for n in
                   ["Scene", "Abstract", "3840 x 2160", "Everyone", "Approved", "Audio responsive",
                    "Media Integration", "Customizable", "Asset Pack"]]
    write(out, "details_one_missing.json", json.dumps(
        {"response": {"result": 1, "resultcount": 2,
                      "publishedfiledetails": [one, {"publishedfileid": str(ID0 + 501), "result": 9}]}},
        ensure_ascii=False) + "\n")

    r = Lcg(7)
    qf = []
    for i in range(5):
        it = detail_item(600 + i, r, described=False)
        it["file_description"] = describe(600 + i)
        it["vote_data"] = {"score": [0.8931, 0.8712, 0.84, 0.7955, 0.5][i],
                           "votes_up": [98, 510, 37, 2204, 3][i], "votes_down": [7, 61, 6, 480, 3][i]}
        it["creator_appid"] = APP
        it["consumer_appid"] = APP
        del it["creator_app_id"], it["consumer_app_id"]
        qf.append(it)
    write(out, "queryfiles_synthetic.json", json.dumps(
        {"response": {"total": 1234567, "publishedfiledetails": qf}}, ensure_ascii=False) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
