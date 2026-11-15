// Wrapper lifetime, part 2: runs after a garbage collection.

test('a node in the tree keeps its wrapper, expandos and listeners', () => {
    const kept = document.getElementById('kept');
    assert.equal(kept.expando.value, 42);
    kept.dispatchEvent(new Event('ping'));
    assert.ok(window.__pinged);
    kept.remove();
});

test('detached unreferenced nodes are collected', () => {
    assert.equal(window.__weakDetached.deref(), undefined);
    assert.equal(window.__weakChild.deref(), undefined);
    assert.equal(window.__weakGone.deref(), undefined);
});

test('strongly referenced detached nodes stay usable', () => {
    assert.equal(window.__strong.textContent, 'strong');
    document.body.appendChild(window.__strong);
    assert.equal(document.body.lastChild, window.__strong);
    window.__strong.remove();
});
