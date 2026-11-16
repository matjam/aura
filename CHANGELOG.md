# Changelog

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
