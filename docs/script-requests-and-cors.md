# Script requests and CORS

This document covers the requests script makes (`fetch`,
`XMLHttpRequest`, module loads, `navigator.sendBeacon`, workers'
requests) and the rules that keep one origin from reading another:
CORS, credentials, redirects, cache modes and the `crossorigin`
attribute on page resources. Read it before changing what script may
send or read.

`fetch`, `XMLHttpRequest`, module loads, `navigator.sendBeacon` and
workers' requests all go through `src/js/fetch.c`, which is where one
origin is kept from reading another. To change what script may send or
read, work there and in `src/net/cors.c` (the rules as functions);
`src/js/prelude_net.js` holds the interfaces (`Headers`, `Request`,
`Response`, `fetch`, `XMLHttpRequest`).

## To try it

Serve two origins from a local HTTP server (`127.0.0.1` and `localhost`
are different origins) and run `aura-host chrome --net --tab URL --do
wait:1500 --do js:CODE`.

## One hop at a time

`fetch.c` asks the embedder (`AuFetcher.start`, `AuFetchRequest` in
`js.h`) for one request with `manual_redirect` set, and gets back the
status and every response header (`au_js_fetch_head`). It follows
redirects itself, so each hop is checked like the first.

The request passes down as `AuResourceRequest` (`src/page/page.h`, sent
by `f_start` in `script.c`), `AuFetchRequest` (`src/browser/page.h`),
`AuLoadRequest` and `AuHttpRequest`, each of which carries the method,
the request headers, the body, `omit_credentials`, `manual_redirect`,
the cache mode and the referrer. A loader without
`au_docview_set_request_ex` can send only a plain GET.

## The rules

The comment at the top of `fetch.c` lists them:

- the forbidden methods and request headers;
- same-origin requests as they are;
- mode `same-origin` fails across origins;
- mode `no-cors` gives an opaque response (status 0, no headers, no
  body) and obeys `Cross-Origin-Resource-Policy`;
- mode `cors` sends `Origin`, sends a preflight `OPTIONS` request when
  the method or a header is not safelisted (or an `XMLHttpRequest` has
  upload listeners), and fails unless the response passes the CORS
  check.

A refused preflight means the request itself is never sent. The
preflight cache is per page (`AuCorsCache`, 32 entries,
`Access-Control-Max-Age` capped at two hours, 5 seconds when absent). A
`cors` response shows script the safelisted headers and those in
`Access-Control-Expose-Headers`; no response shows `Set-Cookie`.

## Credentials

A hop carries cookies and HTTP authentication when the credentials mode
is `include`, or `same-origin` and the hop is same-origin; the network
layer then still applies SameSite and third-party cookie blocking. With
`include` across origins the response needs
`Access-Control-Allow-Credentials: true` and an
`Access-Control-Allow-Origin` that names the origin.

## Redirects

A redirect to another origin makes the request a CORS request from then
on; a cross-origin redirect from a cross-origin URL sends `Origin:
null`; 301, 302 and 303 change the method as HTTP says; `Authorization`
does not cross origins. `redirect: "error"` fails and `"manual"` gives
an `opaqueredirect` response.

## Cache modes and other options

Cache modes map onto the HTTP cache ([`networking.md`](networking.md)):

| Mode | Effect |
|---|---|
| `no-store` | neither reads nor fills it |
| `reload` | fetches again |
| `no-cache` | revalidates |
| `force-cache`, `only-if-cached` | use any stored copy (`only-if-cached` does not fail when there is none) |

`referrer` and `referrerPolicy` are sent as given; `keepalive` is
accepted and changes nothing; `integrity` is checked for sha256, sha384
and sha512 once the body is complete.

## Resources of the page

A script, image or style sheet element with a `crossorigin` attribute
is fetched with `Origin`, and the document view fails the load when the
response does not pass the CORS check (`gate_head` in `docview.c`).
Without the attribute a response of another origin obeys
`Cross-Origin-Resource-Policy`.

A classic script from another origin loaded without CORS runs, and its
errors reach `window.onerror` as "Script error." with no location
(`au_js_mute_script`); the console still shows them. Redirects of these
resources are followed by the network layer and only the final response
is checked.

## Images and a canvas

To decide whether drawing an image taints a canvas, call
`au_docview_image_origin_clean(dv, url)` (`page.h`): it is true for an
image of the document's origin, a `data:` image, and an `<img
crossorigin>` whose response passed the CORS check. An image made by
script and never put in the document is fetched without CORS.
[`canvas.md`](canvas.md) says what tainting does.

## Streams and streaming bodies

To change the Streams API, edit `src/js/streams.js` and run
`tests/js/streams.js` (ctest `js_streams`). To change how a network
body reaches script, see `prelude_net.js` ("A network body as it
arrives") and test with `tests/webapi/test_streams_net.c`.

`streams.js` follows the WHATWG Streams standard: `ReadableStream`
(default and byte streams, default and BYOB readers with `min`, `tee`,
`pipeTo`, `pipeThrough`, async iteration, `ReadableStream.from`),
`WritableStream`, `TransformStream`, the two queuing strategies,
`TextEncoderStream`, `TextDecoderStream`, and `CompressionStream` and
`DecompressionStream` (`gzip`, `deflate`, `deflate-raw`, through
`src/codec` by the natives in `src/js/natives_streams.c`). It is
compiled to bytecode of its own and read the first time script touches
a stream class or a body's stream, as `Intl` is
([`javascript.md`](javascript.md), "Loaded on first use").

A body is a stream on demand:

- `Response.body` and `Request.body` make a stream the first time they
  are read; `text()`, `json()` and the other methods read the body
  without one. `Blob.stream()` gives a byte stream over the blob.
- `new Response(stream)` and `new Request(url, {body: stream, duplex:
  "half"})` take a `ReadableStream`; `clone()` tees it.
- A body from the network reaches script piece by piece as the loader
  delivers it (`au_docscript_resource_data` in `script.c`; the document
  view no longer holds script request bodies until they are complete).
  Until script reads it the pieces are kept in order in a feed; a clone
  gets a feed of its own.

When a body's stream holds 64 KB that its reader has not taken, the
request is paused, and it resumes when the reader pulls again. The
pause goes from `N.fetchPause` through `AuFetcher.pause`,
`AuLoader.pause` and `AuPageFetcher.pause` to `au_http_pause`, which
stops reading the socket; the time a request is paused does not count
toward the 60-second idle timeout. A body read with `text()` or a
clone that nobody reads is not paused: it is kept whole, up to the
loader's 64 MB limit. Canceling a body's stream (or every clone's) ends
the request.

A `fetch` with a stream body reads the stream to its end and then
sends the bytes with a `Content-Length` (see
[`known-gaps.md`](known-gaps.md#script-requests-and-cors)).
`XMLHttpRequest.send` and `navigator.sendBeacon` turn a stream into the
string "[object ReadableStream]", as browsers do.

## EventSource

To change `EventSource`, edit `src/js/eventsource.js` (also loaded on
first use) and the event stream parser in `src/net/sse.c`; run
`tests/js/eventsource.js`, `tests/net/test_sse.c`, `fuzz_sse`
([`fuzzing.md`](fuzzing.md)) and `tests/webapi/test_streams_net.c`.

An `EventSource` is an ordinary script request: mode `cors`,
credentials `include` with `withCredentials` and `same-origin`
without, cache mode `no-store`, `Accept: text/event-stream` and
`Last-Event-ID` after a reconnection. The body goes to the C parser
(`N.sseFeed`) as it arrives, so an event is dispatched as soon as its
blank line comes. The parser drops a line longer than 512 KB and an
event whose data passes 1 MB.

- A response that is not 200 with type `text/event-stream`, or a
  request the same-origin policy or CORS refuses, fails the source: one
  `error` event and `CLOSED`.
- A stream that ends or drops reconnects: `error` with `CONNECTING`,
  then a new request after the reconnection time (3 seconds, or the
  last `retry` field).
- `close()` ends the request at once; a document that goes ends its
  requests with it, and a worker's end with the worker.

A source holds a connection for as long as it lives, and the loader
allows six connections to a host and sixteen in all (on Mac OS 9 each
is an Open Transport endpoint). So a document or worker keeps at most
four sources connected at once; more wait in `CONNECTING`, in the order
they were made, until one closes or drops. A document hidden for Back
and Forward drops its connections and makes them again, with
`Last-Event-ID`, when it is shown, without an `error` event. A server
that sends nothing for 60 seconds hits the idle timeout and the source
reconnects; so does a stream that passes the 64 MB body limit.

## XMLHttpRequest

Asynchronous requests follow the standard's states and events
(`loadstart`, upload events, `readystatechange`, `progress`, `load` or
`error`, `timeout`, `abort`, `loadend`). The body arrives in pieces, so
`progress` and `readystatechange` (state 3) fire as it does, at most
every 50 ms after the first piece, and `responseText` grows.

A synchronous request works for `data:` and `blob:` URLs anywhere and
for network URLs in a worker with its own stack, which waits while the
page runs ([`workers.md`](workers.md)). In a document `send()` throws
NetworkError: delivering network data inside a script call would
re-enter the parser and every loader.

`responseType` `document` parses HTML only.

## Tests

- `tests/webapi/test_cors_net.c`: a browser window over the fake
  transport (what goes out, unreadable responses, the CORS check,
  preflight and its cache, redirects, `crossorigin` scripts, muted
  errors).
- `tests/webapi/test_cors.c`: the rules and the digests.
- `tests/webapi/test_streams_net.c`: slow servers read through
  `Response.body` and `XMLHttpRequest`, a paused request, event stream
  servers (reconnection, `Last-Event-ID`, CORS, failures, a document
  that goes).
- `tests/net/test_sse.c` and `fuzz_sse`: the event stream parser.
- `tests/js/net.js`, `tests/js/streams.js`, `tests/js/eventsource.js`.

## Not done

See [`known-gaps.md`](known-gaps.md#script-requests-and-cors).
