// Wrapper lifetime, part 1 (part 2 runs after a garbage collection).
// A node in the tree keeps its wrapper (identity and expandos); a detached
// node nobody references is collected with its wrapper.
(function () {
    const kept = document.createElement('section');
    kept.id = 'kept';
    kept.expando = { value: 42 };
    kept.addEventListener('ping', () => { window.__pinged = true; });
    document.body.appendChild(kept);

    const detached = document.createElement('div');
    detached.appendChild(document.createElement('span'));
    window.__weakDetached = new WeakRef(detached);
    window.__weakChild = new WeakRef(detached.firstChild);

    // A subtree removed from the document, referenced only weakly.
    const gone = document.createElement('article');
    gone.innerHTML = '<p><b>x</b></p>';
    document.body.appendChild(gone);
    window.__weakGone = new WeakRef(gone.querySelector('b'));
    gone.remove();

    // A detached node kept by a strong reference stays usable.
    window.__strong = document.createElement('aside');
    window.__strong.appendChild(document.createTextNode('strong'));

    // Many short-lived wrappers.
    for (let i = 0; i < 10000; i++) document.createElement('i').textContent = String(i);
})();
