/* config.qml - the settings of the wallpaper type "WalPE" in Plasma's "Desktop and
 * Wallpaper" dialog: what the desktop shows while walpe-daemon shows nothing on the screen. */
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Dialogs as Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kquickcontrols as KQuickControls

Kirigami.FormLayout {
    id: page
    twinFormLayouts: parentLayout    // the form of Plasma's dialog this page is put into
    property alias formLayout: page
    // what Plasma's dialog hands to every wallpaper page (it stops loading the page without them)
    property var configDialog
    property var wallpaperConfiguration
    property alias cfg_Color: colour.color
    property alias cfg_Image: picture.text
    property bool cfg_Under
    property bool cfg_UnderDefault
    property color cfg_ColorDefault
    property string cfg_ImageDefault

    QQC2.Label {
        Layout.maximumWidth: Kirigami.Units.gridUnit * 26
        wrapMode: Text.WordWrap
        text: "The wallpaper is drawn by walpe-daemon and chosen in WalPE. This type only lets it show through between the desktop's icons and widgets."
    }
    KQuickControls.ColorButton {
        id: colour
        Kirigami.FormData.label: "Without WalPE, colour:"
        dialogTitle: "Colour of the desktop while WalPE shows nothing"
    }
    RowLayout {
        Kirigami.FormData.label: "or picture:"
        QQC2.TextField {
            id: picture
            Layout.preferredWidth: Kirigami.Units.gridUnit * 18
            placeholderText: "none (the colour)"
        }
        QQC2.Button {
            icon.name: "document-open"
            text: "Choose..."
            onClicked: chooser.open()
        }
    }
    Dialogs.FileDialog {
        id: chooser
        title: "Picture of the desktop while WalPE shows nothing"
        nameFilters: ["Pictures (*.png *.jpg *.jpeg *.webp *.avif *.jxl *.bmp *.svg)", "All files (*)"]
        onAccepted: picture.text = selectedFile
    }
}
