/* fx_region_models.h - table rows of fx_region.h (included there): the hash of
 * all compiled statements of a pass's two shaders -> the displacement model
 * whose bound was worked out for exactly that text. Made by
 * `fx-region-test --print-models <project dir> <family>` for every pair of
 * shake / waterwaves / iris shader files found in the packages of a
 * library of 88 scenes and every combination of their combos; the
 * comments name the two files by the first 8 hex digits of their SHA-1.
 * A row may only be added after the shader text has been read: see the
 * model descriptions in fx_region.h for what has to hold. */
    /* shake.frag 792c24cb + shake.vert d868aa3e */
    { 0x4f850806a823a060ull, FXR_M_SHAKE },
    { 0xe576153090cc3250ull, FXR_M_SHAKE },
    { 0x77d416ca0271a170ull, FXR_M_SHAKE },
    { 0x580eebe8113b04a0ull, FXR_M_SHAKE },
    { 0x148583d73ce429beull, FXR_M_SHAKE },
    { 0xfdd4b98b8893e04eull, FXR_M_SHAKE },
    { 0xeec613cf6932f0eeull, FXR_M_SHAKE },
    { 0x4033bbc9962c863eull, FXR_M_SHAKE },
    { 0x833294b55291c458ull, FXR_M_SHAKE },
    { 0xb7976bc90e079388ull, FXR_M_SHAKE },
    { 0x2f2fd36e33c52f28ull, FXR_M_SHAKE },
    { 0x661baf70a09bda98ull, FXR_M_SHAKE },
    { 0x6acd049e17f82146ull, FXR_M_SHAKE },
    { 0x2ba4e4bee500cb16ull, FXR_M_SHAKE },
    { 0xf2e9d0d9b42ad336ull, FXR_M_SHAKE },
    { 0x915df369f13341c6ull, FXR_M_SHAKE },
    { 0xa3a5ba15a124a7caull, FXR_M_SHAKE },
    { 0x907b7e13f86fba3aull, FXR_M_SHAKE },
    { 0x50cbeb74cb241e5aull, FXR_M_SHAKE },
    { 0x9e41f644cf34f00aull, FXR_M_SHAKE },
    { 0x5c08e51112af7564ull, FXR_M_SHAKE },
    { 0x3bba5c38683122b4ull, FXR_M_SHAKE },
    { 0x0e5d49934507f8d4ull, FXR_M_SHAKE },
    { 0x0b7bc97b15d249e4ull, FXR_M_SHAKE },
    /* shake.frag 296c064a + shake.vert d868aa3e */
    { 0x6f0b0627b1f311cfull, FXR_M_SHAKE },
    { 0x177d66e7277daa03ull, FXR_M_SHAKE },
    { 0xf239453e4d96e447ull, FXR_M_SHAKE },
    { 0x71537aae6829fadbull, FXR_M_SHAKE },
    { 0xc59ba22a813e3d9full, FXR_M_SHAKE },
    { 0x403b893f5a222c13ull, FXR_M_SHAKE },
    { 0x04d8ab654e1bdf37ull, FXR_M_SHAKE },
    { 0x2f9235d2f4e5c5cbull, FXR_M_SHAKE },
    { 0x1e25ba5dabc4d3a5ull, FXR_M_SHAKE },
    { 0xa74f213c7887dfc1ull, FXR_M_SHAKE },
    { 0x48614b3ee629db29ull, FXR_M_SHAKE },
    { 0x2c24d8dbdf4a3975ull, FXR_M_SHAKE },
    /* shake.frag 0f9e4af6 + shake.vert d868aa3e */
    /* shake.frag b99d245d + shake.vert d8129216 */
    /* shake.frag 4fbc822c + shake.vert 733e925b */
    { 0x529183daae404001ull, FXR_M_SHAKE_OLD },
    { 0xbcda1a0b880cbd17ull, FXR_M_SHAKE_OLDN },
    /* shake.frag f2c1cbd0 + shake.vert 733e925b */
    { 0x132e14ece916706eull, FXR_M_SHAKE_OLD },
    { 0x81cf43c71da32c32ull, FXR_M_SHAKE_OLDN },
    /* waterwaves.frag 31461cc1 + waterwaves.vert e68b8711 */
    { 0x763fa1e5bc0f4b6eull, FXR_M_WAVES },
    { 0x1965a4a8bec624beull, FXR_M_WAVES },
    { 0x1848427d71180a26ull, FXR_M_WAVES },
    { 0x1352b957f6b3996dull, FXR_M_WAVES },
    { 0xac0daff822e84200ull, FXR_M_WAVES },
    { 0xd3d8c2426fa2f3f6ull, FXR_M_WAVES },
    { 0x1acb796e62a7c99eull, FXR_M_WAVES },
    { 0x4be047209625c6d5ull, FXR_M_WAVES },
    { 0x60be8de04f0212f5ull, FXR_M_WAVES2 },
    { 0xcac3332e91e3201dull, FXR_M_WAVES2 },
    { 0xfa922c6352419111ull, FXR_M_WAVES2 },
    { 0xccc1fba977cac12aull, FXR_M_WAVES2 },
    { 0x5e49ef8b7716c600ull, FXR_M_WAVES2 },
    { 0x822febebcf400d32ull, FXR_M_WAVES2 },
    { 0xf0f18297dd94e3b8ull, FXR_M_WAVES2 },
    { 0x297a3b2600f5d40dull, FXR_M_WAVES2 },
    /* waterwaves.frag 0aaa26a1 + waterwaves.vert 00e95e85 */
    { 0x8e87dddf84d5d3a9ull, FXR_M_WAVES_OLD },
    { 0xff33f988e3e1af1eull, FXR_M_WAVES_OLD },
    /* waterwaves.frag 294f6dfb + waterwaves.vert 37e764d7 */
    { 0xcbb71a466ff62031ull, FXR_M_WAVES_OLD },
    { 0x04863e7332b631c7ull, FXR_M_WAVES_OLD },
    { 0x73a7b1c787de8917ull, FXR_M_WAVES_OLD },
    { 0xb9e173dc3ba4d2e2ull, FXR_M_WAVES_OLD },
    /* waterwaves.frag 01812944 + waterwaves.vert aad94f41 */
    { 0x72a66afabef510afull, FXR_M_WAVES_OLD },
    /* waterwaves.frag 971bbf7b + waterwaves.vert 46640181 */
    /* waterwaves.frag 971bbf7b + waterwaves.vert 00e95e85 */
    /* iris.frag e00c9569 + iris.vert 968c5956 */
    { 0xc3c3d7d0e7e5db43ull, FXR_M_IRIS },
    { 0xf5c833200994dcdfull, FXR_M_IRIS },
    { 0xd353885bc5d0b193ull, FXR_M_IRIS },
    /* iris.frag e663281c + iris.vert 545ee4ff */
    { 0x5822fe83501e8063ull, FXR_M_IRIS },
    { 0x0069864ea181623full, FXR_M_IRIS },
    { 0xbc8ce5b50657bf8bull, FXR_M_IRIS },
    /* workshop 2798319181 bokeh_blur: gaussian.frag ab6ddc62 + gaussian.vert, VERTICAL=1, MODE=1,
     * BLENDMODE=0 (the pass that mixes the blur over the effect's input): a gate, see FXR_M_DOF_MIX */
    { 0xedd319c5dd9ca537ull, FXR_M_DOF_MIX },
