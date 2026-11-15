// A deferred script has no insertion point: document.write is ignored.
const before = document.body.innerHTML;
document.write('<p id="fromdefer">x</p>');
test('document.write from a deferred script is ignored', () => {
    assert.equal(document.getElementById('fromdefer'), null);
    assert.equal(document.body.innerHTML, before);
});
