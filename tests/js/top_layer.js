// The top layer: modal dialogs and popovers (HTML "the dialog element",
// "the popover attribute", "inert subtrees"). Expected values come from
// the HTML Standard.

function make(html) {
    const host = document.createElement('div');
    host.innerHTML = html;
    document.body.appendChild(host);
    return host;
}

test('inert attribute: not focusable, reflected', () => {
    const host = make('<div id=ia inert><button id=ib>b</button></div><button id=ic>c</button>');
    const b = document.getElementById('ib'), c = document.getElementById('ic');
    assert.ok(document.getElementById('ia').inert);
    b.focus();
    assert.notEqual(document.activeElement, b, 'an inert button took the focus');
    c.focus();
    assert.equal(document.activeElement, c);
    document.getElementById('ia').inert = false;
    b.focus();
    assert.equal(document.activeElement, b, 'focus after inert was removed');
    b.blur();
    host.remove();
});

test('a modal dialog makes the rest of the document inert', () => {
    const host = make('<button id=mo>outside</button><dialog id=md><input id=mi1><input id=mi2></dialog>');
    const d = document.getElementById('md'), outside = document.getElementById('mo');
    outside.focus();
    assert.equal(document.activeElement, outside);
    d.showModal();
    assert.equal(document.activeElement, document.getElementById('mi1'), 'showModal focuses the first control');
    outside.focus();
    assert.equal(document.activeElement, document.getElementById('mi1'), 'focus() outside the modal dialog');
    d.close();
    assert.equal(document.activeElement, outside, 'close() returns the focus');
    outside.blur();
    host.remove();
});

test('autofocus in a modal dialog', () => {
    const host = make('<dialog id=ad><input id=a1><input id=a2 autofocus></dialog>');
    const d = document.getElementById('ad');
    d.showModal();
    assert.equal(document.activeElement, document.getElementById('a2'));
    d.close();
    host.remove();
});

test('a modal dialog removed from the document leaves the top layer', () => {
    const host = make('<dialog id=rd></dialog>');
    const d = document.getElementById('rd');
    d.showModal();
    assert.ok(d.matches(':modal'));
    d.remove();
    assert.ok(!d.matches(':modal'), 'still modal after removal');
    assert.ok(d.open, 'open stays');
    host.appendChild(d);
    assert.ok(!d.matches(':modal'), 'modal again after insertion');
    d.close();
    /* moving an ancestor out takes it out too */
    d.showModal();
    host.remove();
    assert.ok(!d.matches(':modal'), 'still modal after its ancestor was removed');
    const b = document.createElement('button');
    document.body.appendChild(b);
    b.focus();
    assert.equal(document.activeElement, b, 'the document stayed inert');
    b.remove();
});

test('requestClose fires cancel, and close unless cancelled', () => {
    const host = make('<dialog id=cd></dialog>');
    const d = document.getElementById('cd');
    const log = [];
    d.addEventListener('cancel', (e) => { log.push('cancel'); if (d.keep) e.preventDefault(); });
    d.addEventListener('close', () => log.push('close'));
    d.showModal();
    d.keep = true;
    d.requestClose();
    assert.ok(d.open);
    d.keep = false;
    d.requestClose('x');
    assert.ok(!d.open);
    assert.equal(log.join(' '), 'cancel cancel close');
    assert.equal(d.returnValue, 'x');
    host.remove();
});

test('popover attribute and methods', () => {
    const host = make('<div id=pa popover></div><div id=pm popover=manual></div><div id=px popover=bogus></div><div id=pn></div>');
    const a = document.getElementById('pa'), m = document.getElementById('pm');
    assert.equal(a.popover, 'auto');
    assert.equal(m.popover, 'manual');
    assert.equal(document.getElementById('px').popover, 'manual');
    assert.equal(document.getElementById('pn').popover, null);
    assert.throws(() => document.getElementById('pn').showPopover(), 'NotSupportedError');
    assert.ok(!a.matches(':popover-open'));
    a.showPopover();
    assert.ok(a.matches(':popover-open'));
    a.showPopover(); /* already showing: nothing */
    a.hidePopover();
    assert.ok(!a.matches(':popover-open'));
    assert.equal(a.togglePopover(), true);
    assert.equal(a.togglePopover(true), true);
    assert.equal(a.togglePopover(), false);
    /* removing the attribute hides it */
    m.showPopover();
    m.removeAttribute('popover');
    assert.ok(!m.matches(':popover-open'));
    /* removal from the document hides it */
    a.showPopover();
    a.remove();
    assert.ok(!a.matches(':popover-open'));
    const detached = document.createElement('div');
    detached.popover = 'auto';
    assert.throws(() => detached.showPopover(), 'InvalidStateError');
    host.remove();
});

test('auto popovers close each other, unless nested; manual ones stay', () => {
    const host = make('<div id=o1 popover><div id=o2 popover></div></div><div id=o3 popover></div><div id=om popover=manual></div>');
    const [o1, o2, o3, om] = ['o1', 'o2', 'o3', 'om'].map((id) => document.getElementById(id));
    om.showPopover();
    o1.showPopover();
    o2.showPopover();
    assert.ok(o1.matches(':popover-open') && o2.matches(':popover-open'), 'a nested popover closed its ancestor');
    o3.showPopover();
    assert.ok(!o1.matches(':popover-open') && !o2.matches(':popover-open'), 'auto popovers stayed open');
    assert.ok(om.matches(':popover-open'), 'a manual popover closed');
    o3.hidePopover();
    om.hidePopover();
    host.remove();
});

test('beforetoggle can cancel showing', () => {
    const host = make('<div id=bt popover></div>');
    const p = document.getElementById('bt');
    const log = [];
    p.addEventListener('beforetoggle', (e) => { log.push(e.oldState + '>' + e.newState); if (p.stop) e.preventDefault(); });
    p.stop = true;
    p.showPopover();
    assert.ok(!p.matches(':popover-open'));
    p.stop = false;
    p.showPopover();
    p.hidePopover();
    assert.equal(log.join(' '), 'closed>open closed>open open>closed');
    host.remove();
});

promise_test('toggle fires after the change, coalesced', () => {
    const host = make('<div id=tg popover></div>');
    const p = document.getElementById('tg');
    const log = [];
    p.addEventListener('toggle', (e) => log.push(e.oldState + '>' + e.newState));
    p.showPopover();
    assert.equal(log.length, 0, 'toggle fired synchronously');
    return new Promise((resolve) => setTimeout(resolve, 10)).then(() => {
        assert.equal(log.join(' '), 'closed>open');
        p.hidePopover();
        p.showPopover();
        p.hidePopover();
        return new Promise((resolve) => setTimeout(resolve, 10));
    }).then(() => {
        assert.equal(log.join(' '), 'closed>open open>closed');
        host.remove();
    });
});

test('popovertarget buttons', () => {
    const host = make('<button id=tb popovertarget=tp>t</button><button id=sb popovertarget=tp popovertargetaction=show>s</button>' +
        '<button id=hb popovertarget=tp popovertargetaction=hide>h</button><div id=tp popover></div>' +
        '<form><button id=fb popovertarget=tp>submit</button></form>');
    const p = document.getElementById('tp'), tb = document.getElementById('tb');
    assert.equal(tb.popoverTargetElement, p);
    assert.equal(tb.popoverTargetAction, 'toggle');
    assert.equal(document.getElementById('fb').popoverTargetElement, p, 'reflection');
    tb.click();
    assert.ok(p.matches(':popover-open'), 'toggle shows');
    document.getElementById('sb').click();
    assert.ok(p.matches(':popover-open'), 'show keeps it');
    tb.click();
    assert.ok(!p.matches(':popover-open'), 'toggle hides');
    document.getElementById('hb').click();
    assert.ok(!p.matches(':popover-open'));
    document.getElementById('sb').click();
    document.getElementById('hb').click();
    assert.ok(!p.matches(':popover-open'), 'hide hides');
    const fb = document.getElementById('fb');
    fb.form.addEventListener('submit', (e) => e.preventDefault());
    fb.click();
    assert.ok(!p.matches(':popover-open'), 'a submit button in a form submits instead');
    host.remove();
});

test('showModal hides auto popovers and refuses a showing popover dialog', () => {
    const host = make('<div id=hp popover></div><dialog id=hd></dialog><dialog id=pd popover></dialog>');
    const p = document.getElementById('hp'), d = document.getElementById('hd'), pd = document.getElementById('pd');
    p.showPopover();
    d.showModal();
    assert.ok(!p.matches(':popover-open'), 'the popover stayed open');
    d.close();
    pd.showPopover();
    assert.throws(() => pd.showModal(), 'InvalidStateError');
    pd.hidePopover();
    host.remove();
});
