// Date with a clock and zone supplied by the embedder. Run with
// --clock 1700000000000 --zone -28800 (see CMakeLists.txt): the time is
// 2023-11-14T22:13:20.000Z and local time is 8 hours behind UTC.

const T = 1700000000000;

test('Date.now and new Date read the embedder clock', () => {
    assert.equal(Date.now(), T);
    assert.equal(new Date().getTime(), T);
    assert.equal(new Date().toISOString(), '2023-11-14T22:13:20.000Z');
    assert.equal(Date().slice(0, 24), 'Tue Nov 14 2023 14:13:20');
});

test('performance.timeOrigin is the embedder clock', () => {
    assert.equal(performance.timeOrigin, T);
    assert.equal(performance.timing.navigationStart, T);
});

test('getTimezoneOffset is the embedder zone', () => {
    assert.equal(new Date().getTimezoneOffset(), 480);
    // Fixed for every date: Mac OS 9 has no zone database.
    assert.equal(new Date(Date.UTC(2023, 6, 1)).getTimezoneOffset(), 480);
    assert.equal(new Date(0).getTimezoneOffset(), 480);
});

test('local fields and strings use the zone', () => {
    const d = new Date(T);
    assert.equal(d.getHours(), 14);
    assert.equal(d.getUTCHours(), 22);
    assert.equal(d.getDate(), 14);
    assert.equal(d.getDay(), 2);
    assert.equal(d.toString(), 'Tue Nov 14 2023 14:13:20 GMT-0800');
    assert.equal(d.toDateString(), 'Tue Nov 14 2023');
    assert.equal(d.toLocaleString(), '11/14/2023, 02:13:20 PM');
    assert.equal(d.toUTCString(), 'Tue, 14 Nov 2023 22:13:20 GMT');
    // Local midnight is 08:00 UTC, and the date line is crossed correctly.
    assert.equal(new Date(2023, 10, 15).getTime(), Date.UTC(2023, 10, 15, 8));
    assert.equal(new Date(Date.UTC(2023, 10, 15, 3)).getDate(), 14);
    assert.equal(Date.parse('2023-11-14T14:13:20'), T);
    const e = new Date(T);
    e.setHours(0, 0, 0, 0);
    assert.equal(e.toISOString(), '2023-11-14T08:00:00.000Z');
});
