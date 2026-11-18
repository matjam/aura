# Changelog

## [0.8.0](https://github.com/matjam/aura/compare/v0.7.0...v0.8.0) (2026-11-18)


### Features

* **browser:** an image memory preference, Automatic from the memory free ([01f17a7](https://github.com/matjam/aura/commit/01f17a7349a37a2f50f0f91f4b53ee09329391c9))
* **browser:** keep holding the first paint while head sheets still arrive ([18805ee](https://github.com/matjam/aura/commit/18805eee47db91af505b1cf389adb67d124248c4))
* **browser:** long style passes park so the window scrolls ([a4c4424](https://github.com/matjam/aura/commit/a4c4424b57c57935beb228c6ec875c25b3f5adeb))
* **browser:** the user does not scroll a page whose root hides overflow ([10d9155](https://github.com/matjam/aura/commit/10d9155424c67f148d7e3ffe41f01c4126a92712))
* **gtk,cocoa:** park long scripts on the main thread with ucontext ([7a15d06](https://github.com/matjam/aura/commit/7a15d0657bdd4d3cb7b42cc4d595761a152affe3))


### Performance Improvements

* **browser:** refresh a parked page's view every 250 ms ([855242b](https://github.com/matjam/aura/commit/855242ba86f3a0ef0148f426ee3c801b1bc94247))
* **css:** restyle only what added sheets and changed values reach ([b807370](https://github.com/matjam/aura/commit/b807370422c2ee6b3ff896818f05d35b443434f8))
* **js:** size small shared shapes to what they hold ([23cd10f](https://github.com/matjam/aura/commit/23cd10f086c4a287fb87dc848110ab98811ee80c))
* **paint:** fixed-point transformed layers, smaller image decodes, faster CRC ([b6329f5](https://github.com/matjam/aura/commit/b6329f5f2a5dc7d69ad5b6fc65009fc61a162c96))

## [0.7.0](https://github.com/matjam/aura/compare/v0.6.0...v0.7.0) (2026-11-18)


### Features

* **browser:** connect the history API to the back and forward list ([a99c1db](https://github.com/matjam/aura/commit/a99c1db983e62ec46e0b885fa455c4d7bdfc5735))
* **browser:** hold a new page's first paint until its style sheets arrive ([00abea8](https://github.com/matjam/aura/commit/00abea86b2c2eee6cbb46723048567a28f6e8408))
* **browser:** queue clicks and keys while a page's script is parked ([7d4ebe5](https://github.com/matjam/aura/commit/7d4ebe5a0ca3a2560e4e487368b9466ce7083ccf))
* **browser:** scroll and paint while a page's script runs ([8a5b2e4](https://github.com/matjam/aura/commit/8a5b2e4bde54fc86b941da695842bf25232eff21))
* **browser:** show a long task's progress while its script is parked ([2e2c851](https://github.com/matjam/aura/commit/2e2c851fd0542843502417e12f618dac39931779))
* **css:** CSS.registerProperty ([c1b2e0c](https://github.com/matjam/aura/commit/c1b2e0c76565bb48e87a2b290609f6a27e4097f9))
* **css:** relative colours and the properties sites test for ([bab3f6c](https://github.com/matjam/aura/commit/bab3f6cc884dd54e2848bcc7e54d5a01f1ffd7d9))
* **css:** scroll snapping ([1c07083](https://github.com/matjam/aura/commit/1c070831d0b56e141cd41c63b4c2b1de6c35d5a8))
* **js:** add the small APIs pages call without checking ([3602793](https://github.com/matjam/aura/commit/36027938784106a53839ee2c04e1b74af31f7670))
* **js:** ElementInternals for form-associated custom elements ([b9c20ce](https://github.com/matjam/aura/commit/b9c20cebd8f2d014c561656d0a23172a1eff01f0))
* **js:** IndexedDB, in memory per document ([64b4ae6](https://github.com/matjam/aura/commit/64b4ae6aad7b7634e8a62bb084b34e71a3205960))
* **js:** named access on window for ids and named forms and images ([8fd0e02](https://github.com/matjam/aura/commit/8fd0e02366218d63547b5bd6623dacf2d604a432))
* **js:** navigator.clipboard.writeText and user activation ([f4d8830](https://github.com/matjam/aura/commit/f4d8830bd1ceee29a6ebf8e345fe334a2920baad))
* **js:** nested rules in the CSSOM ([88ae0f0](https://github.com/matjam/aura/commit/88ae0f075fe61414ce040720bf0492a0ce19368d))
* **js:** URLPattern ([0213723](https://github.com/matjam/aura/commit/02137232f6f9384c488e2f092593faa485c5f7de))
* **net:** WebSocket ([cbbf553](https://github.com/matjam/aura/commit/cbbf5533c41371e16b036f3ed47d516d5155556c))


### Bug Fixes

* **browser:** lay out a restyled page before its script parks ([0456b07](https://github.com/matjam/aura/commit/0456b07072faa2990803a3bb239c9b064c76e7c2))
* **js:** make HTMLImageElement.decode() wait for the image ([007dd1d](https://github.com/matjam/aura/commit/007dd1da329b5df6056a0a554e33a4d436961af8))
* **js:** Reddit's video player loads and shows its poster ([7bba65e](https://github.com/matjam/aura/commit/7bba65e09af557b844c2c7e1ecc1253ff1313cd5))
* **layout:** the root's overflow scrolls the window, not a box of its own ([77bc223](https://github.com/matjam/aura/commit/77bc22361cd2213b86b59baf9958b9df32cf85ab))
* **page:** fetch preload links and fire their load and error events ([9fbfd44](https://github.com/matjam/aura/commit/9fbfd440a369cd866c90e50efa2459d8f113f339))


### Performance Improvements

* **css:** keep the rule index when shadow roots adopt indexed sheets ([9045589](https://github.com/matjam/aura/commit/90455893b7dfae78144474394b7bce935b29baa5))
* **js:** deflate function source text while nothing reads it ([eec78d4](https://github.com/matjam/aura/commit/eec78d47df5dd6e12733821c8a1cc062e0255e41))
* **js:** try end-anchored patterns only at the end of the input ([e0587f0](https://github.com/matjam/aura/commit/e0587f08fd1f7ba6478d0341608b9c4557087fa3))

## [0.6.0](https://github.com/matjam/aura/compare/v0.5.0...v0.6.0) (2026-11-18)


### Features

* **css:** play infinite transform and opacity animations ([f79ce48](https://github.com/matjam/aura/commit/f79ce4892dee4400bac419960e9a631888eb35ca))
* **js:** fire load and error for images the document loads ([55dede4](https://github.com/matjam/aura/commit/55dede496d9c663f572ee1534d5d4ca1e1c5edd7))


### Bug Fixes

* **browser:** scale the unknown machine's script limits for 64-bit ([cfcc437](https://github.com/matjam/aura/commit/cfcc437e1eace5f9a08f81ea33294ac8a0540597))
* **layout:** apply min-width and max-width content keywords ([ac57dae](https://github.com/matjam/aura/commit/ac57dae720fac92e217572314b01b478481bc940))
* **layout:** count a transformed box's overflow where it is drawn ([517c374](https://github.com/matjam/aura/commit/517c374d95ffd9322510d731c63420fbffe822a9))
* **layout:** size a select as Chrome does on the Mac ([9913c50](https://github.com/matjam/aura/commit/9913c505b81a425db2c0068aceed1701826fc58b))


### Performance Improvements

* **js:** module graphs share each module fetch ([671122a](https://github.com/matjam/aura/commit/671122a2eb55c4ac1e7d4a253421b4ee337fb358))
* **net:** one download for simultaneous requests of a URL ([7315d05](https://github.com/matjam/aura/commit/7315d053a816408874817e1725c03f81f9a747a9))
* **page:** nested documents repaint only what script changed ([2cffd9d](https://github.com/matjam/aura/commit/2cffd9da06af3aca671e0549d88bf23de3f9e55a))

## [0.5.0](https://github.com/matjam/aura/compare/v0.4.1...v0.5.0) (2026-11-18)


### Features

* a friendlier About box with the author, a home page link and quips ([6664db8](https://github.com/matjam/aura/commit/6664db8373496f168a45510fe7436ef6a5c425d7))
* a new start page with search, most visited sites, bookmarks and tips ([b7f04a5](https://github.com/matjam/aura/commit/b7f04a5c412bf73f31e5fa28aac2306665f1a34c))
* add the Streams API, streaming bodies and EventSource ([dcfb681](https://github.com/matjam/aura/commit/dcfb6817fbfd57738c2a6c7295c633107b48fdf5))
* **browser:** an alert when a page's script runs out of memory ([5483321](https://github.com/matjam/aura/commit/5483321d32fc07182691db695f4d37ff2854f99d))
* **browser:** Appearance item in Preferences ([91fbecf](https://github.com/matjam/aura/commit/91fbecf671614d122e94ee913131e3531ba85397))
* **browser:** page benchmark, about:bench and tools/bench.sh ([9319e35](https://github.com/matjam/aura/commit/9319e3568bcd6bdd8c88d31f41ec0c50dfef72a6))
* bundle Noto Sans SC and KR so Chinese, Japanese and Korean text draws ([f97386c](https://github.com/matjam/aura/commit/f97386caf8898d4be6d85144044b4810226a9ecb))
* **cocoa:** list and developer windows ([f6abc98](https://github.com/matjam/aura/commit/f6abc983bcc7296d6019ff166dad05eeddd427b5))
* **cocoa:** native AppKit port for modern macOS, phase 1 ([9561de3](https://github.com/matjam/aura/commit/9561de34918cfa0d8223ef5a730ea6104b1dd7ec))
* **cocoa:** New Tab button follows the last tab ([2943372](https://github.com/matjam/aura/commit/2943372e0dc5c715e5b5950999f6f78c0f1b9fa4))
* **cocoa:** Preferences and About windows ([59a1f87](https://github.com/matjam/aura/commit/59a1f872a583afff26f50752ff3faced005b076a))
* **cocoa:** Safari-style toolbar, address field and status bubble ([b05723a](https://github.com/matjam/aura/commit/b05723a2f9148091b8f8e46adc4250f6bf130546))
* constructed style sheets and adoptedStyleSheets ([3635f69](https://github.com/matjam/aura/commit/3635f6983862820d6f3abb102ae6e05a0eb72b5d))
* crypto.subtle with AES, RSA, ECDSA, ECDH, X25519, PBKDF2 and HKDF ([fdf719d](https://github.com/matjam/aura/commit/fdf719d319c1ac026a5ec08f95e74d7fcec080eb))
* **css:** container queries ([73bb885](https://github.com/matjam/aura/commit/73bb8852b112700e6c55e01a262c841a6a7bfb25))
* **css:** support scrollbar-width ([21d7daf](https://github.com/matjam/aura/commit/21d7daf370cc6271983fb38202863df9304e6138))
* declarative shadow DOM, slot.assign, retargeted hit testing and focus ([431e7ad](https://github.com/matjam/aura/commit/431e7ad04f9ee9905e8011f41dcd036f0e7c90a8))
* GTK4 port for Linux on Wayland, phase 1 ([ec8c477](https://github.com/matjam/aura/commit/ec8c477137ded69f998532109087cddb038509fc))
* **gtk:** add the About box from the shared credits ([e82699b](https://github.com/matjam/aura/commit/e82699b4a9eea273da2708850b679a1e90a32d7e))
* **gtk:** add the Bookmarks, History, Downloads and Cookies windows ([87a14c1](https://github.com/matjam/aura/commit/87a14c1c0f962dcf7bde90a659ae7552463bd01a))
* **gtk:** add the developer windows painted from their models ([8ca86d9](https://github.com/matjam/aura/commit/8ca86d98181121fc9eea7200fe6272f23393b14a))
* **gtk:** add the Preferences window built from the item descriptions ([6b61ac6](https://github.com/matjam/aura/commit/6b61ac6c8acd7675f40ca08a3beabbfe60d2a8c9))
* **gtk:** add the secondary windows' API and the shared dialogs ([53dc0c8](https://github.com/matjam/aura/commit/53dc0c847995656c0bbeba1b5392c1e8ba10a804))
* **gtk:** follow the desktop's light or dark setting ([d272591](https://github.com/matjam/aura/commit/d27259131532ecc3de648787aa62d679351dcd12))
* **gtk:** install rules, desktop file, metainfo and .deb/AppImage packaging script ([a2f211c](https://github.com/matjam/aura/commit/a2f211c84bfd09dcfb7f763631cadc377b63016b))
* **gtk:** list the secondary windows in the Window menu ([1c28acc](https://github.com/matjam/aura/commit/1c28acc7c98e5da2000834fd227d93d9dae8d9c8))
* **gtk:** put the menus behind a main menu button in the header bar ([7367798](https://github.com/matjam/aura/commit/7367798563c3fb7bc104f43c85eaf76a45d95827))
* **gtk:** wire the secondary windows into the GTK4 port ([e3a5004](https://github.com/matjam/aura/commit/e3a500415b5497092c4908d5eca8a4ee94fdab4e))
* **host:** pause:MS action that waits while loads run ([e75b538](https://github.com/matjam/aura/commit/e75b53860f87033e91580aa882e8aad3d7d187d5))
* inert content outside a modal dialog, Escape, and popovers in the top layer ([c5f6dd2](https://github.com/matjam/aura/commit/c5f6dd2f1584a58e14c29ea7ffed0d353bc1f6a1))
* Intl for English, read on first use ([d4de299](https://github.com/matjam/aura/commit/d4de299043bb260795ff3c9f315a87752ac006a7))
* **js:** add PerformanceTiming and PerformanceNavigation ([36664a9](https://github.com/matjam/aura/commit/36664a98ca1a91eb9afd6a938765ae801b48f4c6))
* **js:** document.fonts and a Web Animations API without rendering ([d371b04](https://github.com/matjam/aura/commit/d371b0447580d75eff336223b541d174f9ac3c01))
* **js:** script scrolls inner scroll containers ([b0f51af](https://github.com/matjam/aura/commit/b0f51af55fe3aac4afd623b50b90683a1e257806))
* **layout:** paint the clip property on absolutely positioned boxes ([50d51c8](https://github.com/matjam/aura/commit/50d51c8ea95497892bb9d0170cdcdeddca196b6d))
* **macos9:** Appearance pop-up in Preferences ([27a8b9c](https://github.com/matjam/aura/commit/27a8b9c7560f97e7c2d9c7d0a03cd3463685931a))
* native chrome mode for browser windows, with complete change notices ([28baee8](https://github.com/matjam/aura/commit/28baee8d251a99e9a35639f9333710d65fd906ca))
* paged font faces that read glyph outlines from the file on demand ([87d969b](https://github.com/matjam/aura/commit/87d969b4ea27475ccab85a68ad38af02086e2177))
* **page:** dark colour scheme reaches open pages live ([3b58f19](https://github.com/matjam/aura/commit/3b58f194ced0132f4f38b8cfc4dca75e19e82b71))
* **page:** web font block periods and font-display ([1db584d](https://github.com/matjam/aura/commit/1db584d80e0b6b8164c829423cd26b21da71e86e))
* render shadow trees through the flat tree with scoped styles ([6d1b4c0](https://github.com/matjam/aura/commit/6d1b4c0f7ae08adf78b88e9587c99ea8b69ea98c))
* stream script request bodies and pause them from script ([674ebde](https://github.com/matjam/aura/commit/674ebde186399ae9ff6ea4f47a79ee6413d6d8bb))
* **tools:** add render-compare.sh to compare a page with Chrome ([49d45a1](https://github.com/matjam/aura/commit/49d45a15b4236e911a4ee0d2bee355c73b1c284b))
* **tools:** render-batch.sh compares many pages with Chrome and ranks them ([8d27e27](https://github.com/matjam/aura/commit/8d27e2711c42f80b1ce34fa8610e3c9c25620e65))


### Bug Fixes

* a control value set by script restyles :valid and :invalid ([c21ba65](https://github.com/matjam/aura/commit/c21ba657540ce05ddd42270f5a591b8ddaab110f))
* a repaint-only style change that makes a layer rebuilds the layer lists ([553eb07](https://github.com/matjam/aura/commit/553eb074fcea7b06d9e677d1e2c8d08cb1193637))
* an element adopted from another document keeps no box or view state ([ceae907](https://github.com/matjam/aura/commit/ceae907b9454f7b8ed485196c5cf5aa9cd55e126))
* **browser:** a scheme change reaches pages made before it ([b83f26f](https://github.com/matjam/aura/commit/b83f26fd3ce960ddbc7f6c4eee2f741d0264ce5a))
* **browser:** closing a window whose pages write storage as they unload ([05c9836](https://github.com/matjam/aura/commit/05c98368e9e7bd9bfc9e0eab34f44948910e4376))
* **browser:** enable Paste and the Edit menu in a page's text field ([f3a2b2b](https://github.com/matjam/aura/commit/f3a2b2bb9c1259e42eefad5a8d8b7a23ace7b6a4))
* canvas 2D converts device bounds and image source rectangles with au_f2px ([5b947d9](https://github.com/matjam/aura/commit/5b947d9d099e1bda48f1f4aefdb338eb31409f21))
* cap crypto.subtle key memory, and give blob workers of file pages crypto.subtle ([66a0096](https://github.com/matjam/aura/commit/66a0096d889af9991ad3d391ce95c0334d282e96))
* **chrome:** repaint the address field when the page's address changes ([ef33a32](https://github.com/matjam/aura/commit/ef33a323eaf224691c216af380989b745277b5dc))
* clip hit testing of a stacking context's layers to its overflow clip ([e9fc5a9](https://github.com/matjam/aura/commit/e9fc5a9fa7964c310e533097a18b69b949cc85bb))
* **cocoa:** carry trackpad scroll fractions to the next event ([7afaf09](https://github.com/matjam/aura/commit/7afaf092f383d6776cbef93cf5a7d79f7bb5dd9b))
* **cocoa:** draw the bold and italic faces of system font collections ([81a901d](https://github.com/matjam/aura/commit/81a901d66386dde7de0b27ef5a15088dfa5083b9))
* **cocoa:** sheets, dead keys and Copy in the secondary windows ([5a89cc7](https://github.com/matjam/aura/commit/5a89cc78303336e2b4074f55ea58bd0dad579269))
* **css:** blockify ::before and ::after of flex and grid containers ([2abf466](https://github.com/matjam/aura/commit/2abf466d4f0bb94b4943252cd951a052ae4b517b))
* **css:** compute ::placeholder styles for input and textarea ([51a991b](https://github.com/matjam/aura/commit/51a991bc8a594fe0f8dfa7a8273ce1a7d0500f5a))
* **css:** compute text-align: match-parent from the parent ([63c5a2d](https://github.com/matjam/aura/commit/63c5a2d66f6c9d385514e7ea4d9fd14d12347c7b))
* **css:** resolve env() safe area and keyboard insets to 0px ([63ea460](https://github.com/matjam/aura/commit/63ea4601d01a7f88edf0280942865fac59786949))
* **dom:** assign a slotted &lt;slot&gt; by its slot attribute, not its name ([b99033b](https://github.com/matjam/aura/commit/b99033bb9f3c64c9a6807309a2f92dd0956bcd60))
* fetch web fonts used only by generated content ([a447d84](https://github.com/matjam/aura/commit/a447d841c6269bb9e979f2ae18ba7bca57eb74ed))
* **frames:** Cloudflare's challenge no longer reloads in a loop ([1309ee9](https://github.com/matjam/aura/commit/1309ee9e784b58aa7a4ee6b8d470c4e17001e02a))
* free a PNG decoder's pixel block and fetches started while a page is freed ([e74ce90](https://github.com/matjam/aura/commit/e74ce90e9e039448b65ae7ba3bb7578211f9dc51))
* **gtk:** give the About box a logo the icon theme has ([7857547](https://github.com/matjam/aura/commit/785754785ff51b9aec33d7b4ffd2877e4b2a36f8))
* **gtk:** give the About box Aura's own licence text ([5895454](https://github.com/matjam/aura/commit/58954546ff4ee9fff5bc5c6cb93d2ef876e77b5b))
* **gtk:** keep the list window's selection alive until it is destroyed ([99ee317](https://github.com/matjam/aura/commit/99ee3178a30a78574176d0701daa10abb703598e))
* **gtk:** let the later focus request win within one flush ([5f5ffa0](https://github.com/matjam/aura/commit/5f5ffa07193384fab8cfe51355c7ed847a5e0703))
* **gtk:** open about: and other scheme URLs from the command line as typed ([b20abbe](https://github.com/matjam/aura/commit/b20abbefee7b41e1fbf47c6cadd58b8ea23082f6))
* **gtk:** paint the developer windows' footer after the buttons' border box ([02269cd](https://github.com/matjam/aura/commit/02269cdf376451e3c1871524c38b30fabf59791d))
* **gtk:** rename the list window's sync, which unistd.h declares under gnu99 ([bd6ea9d](https://github.com/matjam/aura/commit/bd6ea9d3ccc7f11269e5075acae28ac607452505))
* **gtk:** show a font family that is not installed in Preferences ([83082db](https://github.com/matjam/aura/commit/83082db1bcbb0731402a4c580adfd6c13fbc5f1f))
* hex digits without sprintf; coro.h names coro_posix.c ([a550dfe](https://github.com/matjam/aura/commit/a550dfe366b2689f9ed07afe793f2c89137d45a1))
* **host:** paint while waiting so background images load ([47759f8](https://github.com/matjam/aura/commit/47759f86792574c3aadfa5a04458858352f517a5))
* **js:** name a module by its whole URL ([400add8](https://github.com/matjam/aura/commit/400add873540eda82c954b8e5339f91e2e35ce9f))
* **js:** naturalWidth of images the document loaded ([e9731fc](https://github.com/matjam/aura/commit/e9731fcc79e05edd32e173bd4026b0a63b6c2cc8))
* **js:** run inserted scripts with async false in insertion order ([6f15b75](https://github.com/matjam/aura/commit/6f15b75b12b0e76a18326b83419127657ca71179))
* **js:** scale the script stack limit with the pointer size ([6eb5283](https://github.com/matjam/aura/commit/6eb5283feb1608903b4ccedba4a9ae7da68b9b4f))
* **layout:** a replaced element with a percentage max-width is compressible ([5b437e2](https://github.com/matjam/aura/commit/5b437e22b4d90c08c124231db20e47abaf20b849))
* **layout:** clip boxes by clip-path basic shapes ([9ba43ee](https://github.com/matjam/aura/commit/9ba43ee8f4d280bed9b37df1dc295170cd9ce0ed))
* **layout:** clip images to their rounded content box ([c4ee93d](https://github.com/matjam/aura/commit/c4ee93d39b7e785b5ba62dac86f8c685ac3df147))
* **layout:** contain-intrinsic-size, and shadows no longer scroll ([5dd32fb](https://github.com/matjam/aura/commit/5dd32fbead1ac293af1e3aaba00fff253b378be4))
* **layout:** percentage columns of a fixed table take what fixed columns leave ([a301bf6](https://github.com/matjam/aura/commit/a301bf6e8b5dd2cde4036c5deb01f975960f81dc))
* **layout:** resolve calc() lengths in clip-path shapes ([e4b4c00](https://github.com/matjam/aura/commit/e4b4c00a920043df52bb49ac2e6ea6ed2514ec09))
* **layout:** size SVG images with only a viewBox from their containing block ([b005952](https://github.com/matjam/aura/commit/b00595271b30464446bc1939676fd03f6070ac0c))
* **layout:** size text inputs as Chrome does ([a69033d](https://github.com/matjam/aura/commit/a69033d151cf9fcac7e6924e434b5c4dba39f625))
* **layout:** use a replaced flex item's min-content width as its automatic minimum ([6c5bd48](https://github.com/matjam/aura/commit/6c5bd486020e1228ec58ea3433fb18a68f88f0bc))
* **layout:** word-break: break-word lowers min-content like anywhere ([48ff05c](https://github.com/matjam/aura/commit/48ff05c722c1753ee4468f9d183957c4c0494af8))
* **mac:** keep each extra temporary memory zone from emptying ([03f4491](https://github.com/matjam/aura/commit/03f44917987d4542b36a2455f1334b7014b8f9dc))
* manual slot pairs survive their document safely; fuzz shadow trees ([d3995de](https://github.com/matjam/aura/commit/d3995dedbe4cb33bc8b595348d09913f052fa7ca))
* **net:** the user agent names the platform Aura runs on ([3a9bc03](https://github.com/matjam/aura/commit/3a9bc03c8c7e7f5ecd3d88c6d37e4afa554351e5))
* **page:** fire load and error on stylesheet links ([0ce6aae](https://github.com/matjam/aura/commit/0ce6aae7de6201413e96afa5a36895609e8c235b))
* **page:** load web faces whose family name is a CSS keyword ([913be9f](https://github.com/matjam/aura/commit/913be9f2a01a9d8169b88e8943a238f8ef429d6e))
* **page:** URLs of any length in links, frames, base, forms and canvas ([b004ba8](https://github.com/matjam/aura/commit/b004ba8cd919179f51515c450ccc8a12702fe394))
* paint modal dialogs in the top layer with their backdrop ([eaa1a63](https://github.com/matjam/aura/commit/eaa1a638696778208e6982ecd2211238855b64a8))
* **paint:** apply opacity to inline boxes ([e377d1d](https://github.com/matjam/aura/commit/e377d1dfb34f2923c774bf1764b2b73e66d2b4ca))
* refuse a compression stream when its codec cannot be made ([8527846](https://github.com/matjam/aura/commit/85278469eaa084fcd197707eb58d5685a692c8ad))
* relayout keeps what a layout from scratch gives in four cases ([e845fbf](https://github.com/matjam/aura/commit/e845fbf9396783d279f049673949d10500e91da5))
* resolve image, font, script and style sheet URLs of any length ([7b61ed0](https://github.com/matjam/aura/commit/7b61ed08877caceabc56c4f38f6cb68ac2766095))
* resolve url() in style sheets against the sheet's URL ([6cf824c](https://github.com/matjam/aura/commit/6cf824cb5154ba26ea08254953103c90e118f53e))
* size SVG backgrounds without a natural size as CSS Images 3 says ([58472a5](https://github.com/matjam/aura/commit/58472a56df6379d5b2e1bd4d88ffe45d16d8ef46))
* snap background colours to whole pixels ([700809e](https://github.com/matjam/aura/commit/700809e0faada8665723cf88341302cf419a1c5e))
* structuredClone and postMessage of a CryptoKey throw DataCloneError ([e4b7899](https://github.com/matjam/aura/commit/e4b78990b0ee4df9f23dbafcf6977a8c48f9e344))
* subtree rebuilds that moved, adopted or hidden elements got wrong ([13c11a1](https://github.com/matjam/aura/commit/13c11a1025599049b1654df28ae8d827d54266c1))
* SVG and MathML elements have style, dataset and tabIndex ([e4912a6](https://github.com/matjam/aura/commit/e4912a624f07ae69bd698042cf029a4a7cfd631c))
* table percentage margins, display: contents restyles, boxless list ([e563b36](https://github.com/matjam/aura/commit/e563b36b38fa614bf868c0ce2a68e36f089f1bee))
* three layouts that differed between a relayout and a whole build ([b2d0211](https://github.com/matjam/aura/commit/b2d02115867effb93769da1026e3925ec3c26f81))
* **tools:** cap aura-host's busy time in render-compare ([c313428](https://github.com/matjam/aura/commit/c3134280f4ad1216c635542a9820e19b8813eb74))
* **tools:** compare with Chrome at the exact viewport size ([c656774](https://github.com/matjam/aura/commit/c656774064d0391dfaaa33ec1e1918146b0b8a0a))
* **tools:** render-compare.sh renders Chrome in the light scheme ([8018150](https://github.com/matjam/aura/commit/8018150c00645ff320c48bf2f93c4cb15db9a336))
* two undefined behaviours CI's sanitizer build reports ([1d07645](https://github.com/matjam/aura/commit/1d0764599a28424124443cad4b113f22d5f005d9))


### Performance Improvements

* a style sheet that arrives late restyles without a whole box tree build ([ad722b0](https://github.com/matjam/aura/commit/ad722b062dbad49da56624783196aa113e2ddf27))
* Collator compares ASCII strings and identical prefixes quickly ([fcb8ad6](https://github.com/matjam/aura/commit/fcb8ad6b7748a0b665a77cb6e733f0698e0af5ef))
* compile each module once while its graph loads ([b9ef953](https://github.com/matjam/aura/commit/b9ef953b8bfd4d09348042bbfe78971c74a95ba6))
* **core:** keep blocks that outlive pages out of the page's heap space ([81bf64d](https://github.com/matjam/aura/commit/81bf64d6e706c2ad60b73201d697cecc08196e02))
* **core:** resize large blocks in place ([a99740d](https://github.com/matjam/aura/commit/a99740d4700bd676a99624d79d4a875d928297f6))
* **css:** share parsed sheets between documents and free atoms with their pages ([ca8ce24](https://github.com/matjam/aura/commit/ca8ce2456379a45ab3e89e1700fe5b61165747a2))
* find scoped variables through the variable hash table ([4330f20](https://github.com/matjam/aura/commit/4330f20c9770bbb611849f8c83cbfe3234a0c51c))
* hold cached bodies and host font files at their size ([33162e3](https://github.com/matjam/aura/commit/33162e352ca5bb1d4d87d571dc161eedbbb91f13))
* **image:** four times the image budget on 64-bit builds ([4fea64b](https://github.com/matjam/aura/commit/4fea64ba02a222b00ed2bd6bc1c5d17cd00a446b))
* **image:** stop decoding a capped image again on every paint ([71a20ee](https://github.com/matjam/aura/commit/71a20eed359c7f644694969c939a55edd2621aa8))
* Intl.Segmenter finds segments as they are asked for ([1b719a8](https://github.com/matjam/aura/commit/1b719a83cdf935baa7071fb201e77957700e5640))
* **js:** compile large scripts in less memory ([3257007](https://github.com/matjam/aura/commit/325700712481bc653c39646ff161faa70c1518f1))
* **js:** read range, cssom and canvas prelude modules on first use ([57421f5](https://github.com/matjam/aura/commit/57421f512c4f584aa77459bccc27e4568099a802))
* keep paged glyphs by glyph rather than by block ([43c6f3f](https://github.com/matjam/aura/commit/43c6f3f365d0fa1dca5dff9a99351783445d9e8c))
* keep parsed selectors for matches, closest and querySelector ([4a5e9cc](https://github.com/matjam/aura/commit/4a5e9cc01fda76ba502b80962f25afec032d112e))
* keep shadow matching out of the hot selector path; document shadow trees ([709e2ad](https://github.com/matjam/aura/commit/709e2ad60d0a7b667b2f22a03c424b860088094f))
* **layout:** scroll steps paint only what changed ([22a4ced](https://github.com/matjam/aura/commit/22a4ced700e2d25f20c375e2eeb7f185c817e1f3))
* **mac:** give large long-lived blocks a handle each ([c099afa](https://github.com/matjam/aura/commit/c099afa5f6424d942a7cedab38e5daf0c650f444))
* **macos9:** word-wise memcpy and memmove in place of newlib's ([80ec243](https://github.com/matjam/aura/commit/80ec2430381364b676084cff643ec97fa1029ecf))
* **page:** repaint only what script changed ([7980dd2](https://github.com/matjam/aura/commit/7980dd2a7f278c43a5ee1e45fcc7581274459f8b))
* **paint:** draw a scaled &lt;img&gt; without a layer ([fb4279e](https://github.com/matjam/aura/commit/fb4279e9d3a2f4c0a0fcdbec3fdb771cb76f476a))
* read the viewport without forcing a layout ([84662f9](https://github.com/matjam/aura/commit/84662f978894f88abf1a76fd68b7e9a128a972fa))
* rebuild only the changed subtree of the box tree after a DOM change ([e329b10](https://github.com/matjam/aura/commit/e329b10794f3c380b02fee8d0919f7585c026a76))
* record the parser's bracket look-ahead instead of repeating it ([bd83c69](https://github.com/matjam/aura/commit/bd83c6923c2d2674fda08763a624c63340c6f1ae))
* return the pool's empty slabs every ten seconds while idle on Mac OS 9 ([ca03101](https://github.com/matjam/aura/commit/ca03101b0d72859cf229fad6a3b89605513fd6df))
* serve blocks of 2 to 16 KB from slabs in the pooled allocator ([f846bee](https://github.com/matjam/aura/commit/f846beece168eed760e2c02de2365719008b3cde))
* size the pool's slabs for blocks over 2 KB near 32 KB ([841998b](https://github.com/matjam/aura/commit/841998b34b2a3acb7fd413ffb3b2463ce2ddf47e))

## [0.4.1](https://github.com/matjam/aura/compare/v0.4.0...v0.4.1) (2026-11-16)


### Bug Fixes

* float to integer conversions a page reaches with huge or NaN values ([53719ec](https://github.com/matjam/aura/commit/53719ec563ae11420114b4daddb3b3898c9bf9f0))
* inline layout and float placement take time in proportion to the page ([4764d4f](https://github.com/matjam/aura/commit/4764d4feabc378e2d693ce781aac6fe8011152ba))
* input handling holds a reference to elements across script ([eb40bbb](https://github.com/matjam/aura/commit/eb40bbb7462810245652fdf9c7446a43efd83570))

## [0.4.0](https://github.com/matjam/aura/compare/v0.3.0...v0.4.0) (2026-11-16)


### Features

* a deflate encoder (raw, zlib and gzip; stored, fixed and dynamic blocks; two levels) ([1825a93](https://github.com/matjam/aura/commit/1825a93f6c2665a721e8686f65353b7f77f228a6))
* background tabs are unloaded when memory is low, and a restored session loads only its active tab ([ecf9852](https://github.com/matjam/aura/commit/ecf9852804edb371277726249522db115c04f144))
* localStorage and sessionStorage are kept by a storage service: shared by an origin's pages and tabs, with storage events, per-tab session storage and document.referrer ([ddf8930](https://github.com/matjam/aura/commit/ddf8930513dbbc9ce868f1a1eadc3b9c0ef3a052))
* localStorage and sessionStorage are stored, shared across an origin's pages and saved; document.referrer ([1971abb](https://github.com/matjam/aura/commit/1971abb88894fed68d84c428ba00c120bffd0ca9))
* Page Timings shows the page source's size, what is held for it and how long compressing took ([76fff4e](https://github.com/matjam/aura/commit/76fff4e91c89e87b2f26b3bf3c0e623c9ea68231))
* web storage in the host tools, the sample page, the file fuzzer and docs/web-storage.md; session storage follows the cookie rules into the Session file ([ea5dbcc](https://github.com/matjam/aura/commit/ea5dbccaa81feb2c93f888af73b991f7dd447b3c))
* web storage is saved in the Local Storage folder, follows the cookie rules, and shows in the Cookies window and System Status ([7be5846](https://github.com/matjam/aura/commit/7be58465dbc24963c86866250c412ee4cff1a708))


### Bug Fixes

* the deflate encoder's tree builder states its two-leaf minimum, for GCC's bounds check ([a32ea9e](https://github.com/matjam/aura/commit/a32ea9e783f602d937318a3439f919c15e7f8f74))
* the kept-document estimate counts a parsed sheet as it is stored now ([03313a7](https://github.com/matjam/aura/commit/03313a72ef6d53ab558946734d9aa072f4e7edc3))


### Performance Improvements

* a hidden document packs its style sheet text at once ([8574ef8](https://github.com/matjam/aura/commit/8574ef871ef81982d7be55a8a6665a35b5e01b13))
* a sheet that script changed keeps its text once ([c809d77](https://github.com/matjam/aura/commit/c809d77b346d010c9c7709502fd855fd3d106413))
* a sheet's text is held deflated once the page has loaded ([1ab0608](https://github.com/matjam/aura/commit/1ab0608babd5afab19df30dc05dc713ab3aaa1f9))
* a small sheet's arena chunks stay small, and the Mac OS 9 build checks the record sizes ([dafce20](https://github.com/matjam/aura/commit/dafce201354370961481149adeb259420d40f8f2))
* kept documents free their layout, background tabs unload when memory is low, page source and cached responses are held compressed ([52d0953](https://github.com/matjam/aura/commit/52d09533f4c1d7e92ca0b80d742c8464d32b5ea7))
* only the last occurrence of a repeated sheet is indexed; a token list is one block; an aliased custom property shares its value ([a077c0b](https://github.com/matjam/aura/commit/a077c0bc937225c82ab7dc89745f5cb1e24ee3db))
* parsed style sheets live in an arena, with packed rules and selectors; blocks that cannot apply are not parsed ([511b534](https://github.com/matjam/aura/commit/511b534e1f8977159821228a6ba5b7abc227fa78))
* parsed style sheets take a fifth of the memory: lazy rule bodies, an arena with packed records, shared and repeated sheets, packed text ([0c30977](https://github.com/matjam/aura/commit/0c3097712edacea62d9c0272a0cc82017b82b68c))
* the HTTP cache holds encoded responses as they arrived and decodes them when served ([2798b18](https://github.com/matjam/aura/commit/2798b18287c71712acee89ae593e3395616e7cf3))
* the page source is held compressed once the load is over ([30ec393](https://github.com/matjam/aura/commit/30ec393edc9d492af326e8d2651e194fbeb7f4f5))
* the rule index rejects on a selector's first filter hash and keeps a parsed block's address; a token list is built in its block ([8d0d7ff](https://github.com/matjam/aura/commit/8d0d7ff215881c81fb6a72939e1f47f28d204f8e))

## 0.3.0 (2026-11-16)


### Features

* crossorigin on script, img and link, Cross-Origin-Resource-Policy, muted errors of cross-origin scripts ([d7b6f39](https://github.com/matjam/aura/commit/d7b6f39e0c6053acbd3c2d154211c6d874ab551e))
* dedicated Web Workers, each a script runtime of its own on a coroutine stack, and crypto.subtle digests and HMAC ([c9a11ae](https://github.com/matjam/aura/commit/c9a11ae6134d603dfe8cd898e126cfa8369e7662))
* fetch and XMLHttpRequest send any method, headers and body, with CORS preflight and redirects in the engine ([581db6b](https://github.com/matjam/aura/commit/581db6b58ffb5b7d91b8dbded1a632d62654495d))
* Help &gt; Report a Problem, System Status, and a tidy Develop menu on Mac OS 9 ([e4bd381](https://github.com/matjam/aura/commit/e4bd381a042b9a34a8646d8c365cb15b9646729d))
* iframe, object and embed documents ([247b614](https://github.com/matjam/aura/commit/247b6143647ea931b334ad1b17e5afcea6597bb8))
* location.ancestorOrigins; test: a 4 s kill timer makes js_limits robust on a busy machine ([b82eba2](https://github.com/matjam/aura/commit/b82eba2da9d9f6c5b72b38d97137a213fd072d20))
* old servers that offer only RSA key exchange load again, as a last resort ([1cb573f](https://github.com/matjam/aura/commit/1cb573fb58d7fb8c84ada69c18309ab6566cb945))
* old servers with RSA key exchange load again, as a last resort ([25a6584](https://github.com/matjam/aura/commit/25a6584391aac5536c0770b7b9c8b4cfd6f35ba7))
* releases as a self-extracting installer (BinHex and MacBinary), made by release-please ([943a0c1](https://github.com/matjam/aura/commit/943a0c148b41e96faf4b8ade6cd90e5c08c719e9))
* Report a Problem, System Status, and a tidier Develop menu and developer windows ([e4828e4](https://github.com/matjam/aura/commit/e4828e4bc205d8b1b62a1b6059bee0c896f78661))
* script memory limits follow the machine and are preferences; document.cookie works; one compile per page for large scripts ([e10b69a](https://github.com/matjam/aura/commit/e10b69a5732bcb6db0e5f44170aae068dabe4500))
* script memory limits in Preferences &gt; Content (Automatic or a custom size), temporary memory when the application heap is short, and out-of-memory in a worker reaches the page ([8711f28](https://github.com/matjam/aura/commit/8711f2872fe2285bb0d38797406782e5ff737100))
* site icons in tabs, crypto for pages, precompiled script prelude, lower memory use ([fb66e2b](https://github.com/matjam/aura/commit/fb66e2bd793b28edfb2a9624337025d101a1cf24))
* the bug report fits a GitHub issue; contributing notes point to Report a Problem ([cf6d89a](https://github.com/matjam/aura/commit/cf6d89ab3cd887c4b5881699dff77dfdb97c372e))
* the bug report, the System Status report and tidier developer reports in the engine ([f45bef2](https://github.com/matjam/aura/commit/f45bef2235cbc50438a1db5780a7dec1ad2286ce))
* the canvas element with a 2D context ([f841c96](https://github.com/matjam/aura/commit/f841c968e59e02c79f57e76cdbaf827689b5eb2e))
* the canvas element with a 2D context, and OffscreenCanvas ([2cb753f](https://github.com/matjam/aura/commit/2cb753ff73cab4718c37a92a8a89d0d7e48a750e))
* Thread Manager coroutines for workers on Mac OS 9; test: a proof of work in a worker while page timers fire ([c297776](https://github.com/matjam/aura/commit/c2977760cf1e55e734bf29db2b40d6d4ddf2ea8b))
* TLS 1.3 with Mbed TLS 4.1 LTS, replacing BearSSL ([5048e50](https://github.com/matjam/aura/commit/5048e50419b5f793e60778d94a3742d8673ff975))
* TLS 1.3 with Mbed TLS 4.1, replacing BearSSL ([60e45a1](https://github.com/matjam/aura/commit/60e45a198c487a251050f3a48cdb248ffd8d0121))
* TLS sessions up to 4 KB, less memory per idle connection, fetch --tls12 and --again ([f8bc350](https://github.com/matjam/aura/commit/f8bc350066e9bb1b0a0f59eb78f99eb4465841a7))
* Web Workers, complete fetch and XMLHttpRequest, CORS, crypto.subtle digests ([8b722e1](https://github.com/matjam/aura/commit/8b722e11baa39d13b427d40cd40fa96d7fea730b))


### Bug Fixes

* a frame's window.name is its iframe's name; frames are known to script from its first statement; workers may use 24 MB ([b66e006](https://github.com/matjam/aura/commit/b66e006c56e75a3a53d2fe8d1af70495e31bc5bf))
* a script right after an iframe finds the frame's window by index and by name, a frame finds its siblings through parent.frames while it loads, and a srcdoc frame resolves URLs against its parent's base ([422a591](https://github.com/matjam/aura/commit/422a591c51ea50773a1c85523c7d5fdd2c3f70ba))
* document.cookie reads and sets the cookie jar; no embedder implemented the script hooks, so it always read empty ([1ddc2ad](https://github.com/matjam/aura/commit/1ddc2ad76855b066ea75c28dfba24f5ef047a69b))
* hangs, overflows and deep-nesting faults found by new fuzzers; a reliable guest launch helper and a smoke test ([0287d69](https://github.com/matjam/aura/commit/0287d69cc9b5bf7db87769dd61e70a4b3c84dd43))
* hash updates accept a null pointer with zero length ([bc43887](https://github.com/matjam/aura/commit/bc43887efe5c613aeb93f7e1a68db0ada905b99f))
* hashing an empty update with a NULL pointer ([3a77835](https://github.com/matjam/aura/commit/3a77835de7098038f5540b79371f1e732267dd2c))
* no empty append of a synchronous request's headers ([d27dce4](https://github.com/matjam/aura/commit/d27dce4c41092ac08256c2b7561d1f66bd920aba))
* no layout rebuild when images arrive, viewport restyle only when needed, layout defects ([c9fb896](https://github.com/matjam/aura/commit/c9fb896eec4c0714f8685ad42ee28463907ef03e))
* no null pointer to memcpy or memcmp for an empty certificate; sanitizer builds check it on every host ([6992b4b](https://github.com/matjam/aura/commit/6992b4b243820201cd15a06a92d7de7b6acd7a0d))
* script's Date uses the Mac's clock and time zone, and Network Status shows the clock ([133b7f6](https://github.com/matjam/aura/commit/133b7f6e1c9133b91f63db7c0604155cfb8e536a))
* script's Date uses the Mac's clock and zone; the test machine's clock is set to the guest zone's local time ([3e8a0d5](https://github.com/matjam/aura/commit/3e8a0d555771be1862cc40e89a1c008d585348d5))
* store computed lengths with zeroed padding; docs for CI and releases ([70ebe03](https://github.com/matjam/aura/commit/70ebe038c604e85bd804f219f800093ab95310dd))
* the rasterizer drops edges too flat to have a finite slope; perf: canvas clearRect and scaled images ([6c977d7](https://github.com/matjam/aura/commit/6c977d7acb2c11da5ecf0362835682d0cc8b13f3))
* the script memory pop-ups in Preferences have menu ids of their own, and their rows fit the pane ([35d1bf5](https://github.com/matjam/aura/commit/35d1bf5c3c2fd9ef1f670005150472488e7716f0))


### Performance Improvements

* a large script is compiled once per page and its bytecode read by the page's other documents and workers; script memory limits come from preferences or the memory free ([5b590a0](https://github.com/matjam/aura/commit/5b590a0e90c1f8b3a0b0cd6a24983562eac54517))
* a script's text is handed to the engine and freed once compiled; a worker's script and importScripts no longer hold the text as bytes, a string and a copy ([448f931](https://github.com/matjam/aura/commit/448f931bc22c9cdb4a0a57946e86ecc0b5726467))
* one source text per script, shared by the documents of a page; out-of-memory console lines name the runtime, its limit and what it was compiling ([86f7a88](https://github.com/matjam/aura/commit/86f7a8888ec489e42f11d4c32555df9ff45db42d))
