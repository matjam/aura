// URLPattern (src/js/urlpattern.js): component patterns, groups and
// modifiers, string patterns split into components, base URLs, exec
// results and the normalized pattern strings.

test('the global', () => {
    assert.equal(typeof URLPattern, 'function');
    const p = new URLPattern({ pathname: '/books' });
    assert.equal(Object.prototype.toString.call(p), '[object URLPattern]');
    assert.equal(p.protocol, '*');
    assert.equal(p.pathname, '/books');
    assert.equal(p.hasRegExpGroups, false);
});

test('fixed paths and named groups', () => {
    const p = new URLPattern({ pathname: '/books/:id' });
    assert.ok(p.test('https://example.com/books/123'));
    assert.ok(!p.test('https://example.com/books'));
    assert.ok(!p.test('https://example.com/books/1/2'));
    const r = p.exec('https://example.com/books/123?x=1#h');
    assert.equal(r.pathname.groups.id, '123');
    assert.equal(r.pathname.input, '/books/123');
    assert.equal(r.search.input, 'x=1');
    assert.equal(r.hash.input, 'h');
    assert.equal(r.inputs[0], 'https://example.com/books/123?x=1#h');
});

test('modifiers', () => {
    const opt = new URLPattern({ pathname: '/books/:id?' });
    assert.ok(opt.test({ pathname: '/books' }));
    assert.ok(opt.test({ pathname: '/books/7' }));
    assert.ok(!opt.test({ pathname: '/books/' }));
    const plus = new URLPattern({ pathname: '/f/:rest+' });
    assert.equal(plus.exec({ pathname: '/f/a/b/c' }).pathname.groups.rest, 'a/b/c');
    assert.ok(!plus.test({ pathname: '/f' }));
    const star = new URLPattern({ pathname: '/f/:rest*' });
    assert.ok(star.test({ pathname: '/f' }));
    assert.equal(star.exec({ pathname: '/f/x/y' }).pathname.groups.rest, 'x/y');
    assert.equal(opt.exec({ pathname: '/books' }).pathname.groups.id, undefined);
});

test('wildcards and regexp groups', () => {
    const w = new URLPattern({ pathname: '/static/*' });
    assert.equal(w.exec({ pathname: '/static/a/b.js' }).pathname.groups[0], 'a/b.js');
    const r = new URLPattern({ pathname: '/post/:id(\\d+)' });
    assert.ok(r.hasRegExpGroups);
    assert.ok(r.test({ pathname: '/post/42' }));
    assert.ok(!r.test({ pathname: '/post/abc' }));
    const anon = new URLPattern({ pathname: '/(foo|bar)/x' });
    assert.equal(anon.exec({ pathname: '/bar/x' }).pathname.groups[0], 'bar');
});

test('brace groups', () => {
    const p = new URLPattern({ pathname: '/product{/}?' });
    assert.ok(p.test({ pathname: '/product' }));
    assert.ok(p.test({ pathname: '/product/' }));
    const q = new URLPattern({ pathname: '/blog/:slug{.html}?' });
    assert.equal(q.exec({ pathname: '/blog/hello.html' }).pathname.groups.slug, 'hello');
    assert.equal(q.exec({ pathname: '/blog/hello' }).pathname.groups.slug, 'hello');
});

test('string patterns', () => {
    const p = new URLPattern('https://example.com/books/:id');
    assert.equal(p.protocol, 'https');
    assert.equal(p.hostname, 'example.com');
    assert.equal(p.pathname, '/books/:id');
    assert.ok(p.test('https://example.com/books/5'));
    assert.ok(!p.test('http://example.com/books/5'));
    assert.ok(!p.test('https://other.com/books/5'));
    const h = new URLPattern('https://*.example.com/*');
    assert.ok(h.test('https://cdn.example.com/a/b'));
    assert.ok(!h.test('https://example.org/'));
    const q = new URLPattern('https://example.com/search?q=:term');
    assert.equal(q.exec('https://example.com/search?q=cats').search.groups.term, 'cats');
});

test('relative patterns and base URLs', () => {
    const p = new URLPattern('/api/:v/*', 'https://example.com');
    assert.equal(p.hostname, 'example.com');
    assert.ok(p.test('https://example.com/api/2/users'));
    assert.ok(!p.test('https://example.net/api/2/users'));
    assert.ok(p.test('/api/1/x', 'https://example.com'));
    assert.throws(() => new URLPattern('/no/base'), 'TypeError');
    const d = new URLPattern({ pathname: '/a', baseURL: 'https://example.com/x/y' });
    assert.equal(d.protocol, 'https');
    assert.equal(d.hostname, 'example.com');
});

test('default ports', () => {
    const p = new URLPattern({ protocol: 'https', port: '443' });
    assert.equal(p.port, '');
    assert.ok(p.test('https://example.com:443/'));
    assert.ok(!p.test('https://example.com:8443/'));
});

test('ignoreCase', () => {
    const p = new URLPattern({ pathname: '/About' }, { ignoreCase: true });
    assert.ok(p.test({ pathname: '/about' }));
    assert.ok(!new URLPattern({ pathname: '/About' }).test({ pathname: '/about' }));
});

test('errors', () => {
    assert.throws(() => new URLPattern({ pathname: '/:' }), 'TypeError');
    assert.throws(() => new URLPattern({ pathname: '/(' }), 'TypeError');
    assert.throws(() => new URLPattern({ pathname: '/:a/:a' }), 'TypeError');
    assert.throws(() => new URLPattern({ pathname: '/{x' }), 'TypeError');
    assert.throws(() => new URLPattern({ pathname: '/(a(b))' }), 'TypeError');
    assert.ok(!new URLPattern({ pathname: '/x' }).test('not a url'));
});

test('pattern strings round trip', () => {
    for (const s of ['/books/:id', '/static/*', '/post/:id(\\d+)', '/f/:rest+', '/product{/}?'])
        assert.equal(new URLPattern({ pathname: s }).pathname, s, s);
});
