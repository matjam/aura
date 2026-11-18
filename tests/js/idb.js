// IndexedDB (src/js/idb.js): opening and upgrading, stores and indexes,
// keys and key ranges, cursors, transactions that commit, abort and roll
// back, and the events that bubble from a request to its database.

const req = (r) => new Promise((resolve, reject) => {
    r.onsuccess = () => resolve(r.result);
    r.onerror = () => reject(r.error);
});
let dbn = 0;
/* Opens a fresh database, running upgrade(db, tx) in its upgrade. */
function openDb(upgrade, version = 1) {
    const r = indexedDB.open('t' + (++dbn), version);
    r.onupgradeneeded = (e) => upgrade(r.result, r.transaction, e);
    return req(r);
}
const txDone = (tx) => new Promise((resolve, reject) => {
    tx.oncomplete = () => resolve('complete');
    tx.onabort = () => reject(tx.error || new Error('abort'));
});

test('the globals', () => {
    assert.equal(typeof indexedDB, 'object');
    assert.equal(Object.prototype.toString.call(indexedDB), '[object IDBFactory]');
    for (const n of ['IDBFactory', 'IDBRequest', 'IDBOpenDBRequest', 'IDBDatabase', 'IDBTransaction', 'IDBObjectStore',
        'IDBIndex', 'IDBCursor', 'IDBCursorWithValue', 'IDBKeyRange', 'IDBVersionChangeEvent'])
        assert.equal(typeof globalThis[n], 'function', n);
    assert.throws(() => new IDBDatabase(), 'TypeError');
    assert.throws(() => indexedDB.open('x', 0), 'TypeError');
});

test('keys compare in type order', () => {
    const c = (a, b) => indexedDB.cmp(a, b);
    assert.equal(c(1, 2), -1);
    assert.equal(c(2, 2), 0);
    assert.equal(c(-Infinity, 0), -1);
    assert.equal(c(1e9, new Date(0)), -1);
    assert.equal(c(new Date(5), 'a'), -1);
    assert.equal(c('b', 'a'), 1);
    assert.equal(c('zz', new Uint8Array([0])), -1);
    assert.equal(c(new Uint8Array([1, 2]), new Uint8Array([1, 3]).buffer), -1);
    assert.equal(c(new Uint8Array([1]), [0]), -1);
    assert.equal(c([1, 'a'], [1, 'b']), -1);
    assert.equal(c([1], [1, 0]), -1);
    assert.throws(() => c(NaN, 1), 'DataError');
    assert.throws(() => c({}, 1), 'DataError');
    assert.throws(() => c(null, 1), 'DataError');
    assert.throws(() => c(true, 1), 'DataError');
    assert.throws(() => c(new Date(NaN), 1), 'DataError');
});

test('key ranges', () => {
    const r = IDBKeyRange.bound(1, 5, true, false);
    assert.equal(r.lower, 1);
    assert.equal(r.upper, 5);
    assert.equal(r.lowerOpen, true);
    assert.equal(r.includes(1), false);
    assert.equal(r.includes(5), true);
    assert.equal(IDBKeyRange.only('a').includes('a'), true);
    assert.equal(IDBKeyRange.lowerBound(3).includes(100), true);
    assert.equal(IDBKeyRange.upperBound(3, true).includes(3), false);
    assert.throws(() => IDBKeyRange.bound(5, 1), 'DataError');
    assert.throws(() => IDBKeyRange.bound(1, 1, true), 'DataError');
    assert.throws(() => new IDBKeyRange(), 'TypeError');
});

promise_test('open, upgrade, put and get', async () => {
    let old = -1, nv = -1;
    const db = await openDb((db, tx, e) => {
        old = e.oldVersion;
        nv = e.newVersion;
        assert.ok(e instanceof IDBVersionChangeEvent);
        assert.equal(tx.mode, 'versionchange');
        const s = db.createObjectStore('books', { keyPath: 'isbn' });
        s.createIndex('by_title', 'title', { unique: true });
        s.createIndex('by_author', 'author');
        assert.throws(() => db.createObjectStore('books'), 'ConstraintError');
    });
    assert.equal(old, 0);
    assert.equal(nv, 1);
    assert.equal(db.version, 1);
    assert.equal(db.objectStoreNames.length, 1);
    assert.ok(db.objectStoreNames.contains('books'));
    const tx = db.transaction('books', 'readwrite');
    const s = tx.objectStore('books');
    assert.deepEqual(Array.from(s.indexNames), ['by_author', 'by_title']);
    s.put({ title: 'Quarry Memories', author: 'Fred', isbn: 123456 });
    s.put({ title: 'Water Buffaloes', author: 'Fred', isbn: 234567 });
    s.put({ title: 'Bedrock Nights', author: 'Barney', isbn: 345678 });
    assert.equal(await txDone(tx), 'complete');
    const r = db.transaction('books').objectStore('books');
    const book = await req(r.get(234567));
    assert.equal(book.title, 'Water Buffaloes');
    assert.equal(await req(r.index('by_title').getKey('Bedrock Nights')), 345678);
    const fred = await req(db.transaction('books').objectStore('books').index('by_author').getAll('Fred'));
    assert.deepEqual(fred.map((b) => b.isbn), [123456, 234567]);
    assert.equal(await req(db.transaction('books').objectStore('books').count()), 3);
    assert.equal(await req(db.transaction('books').objectStore('books').get(1)), undefined);
    db.close();
});

promise_test('stored values are copies', async () => {
    const db = await openDb((db) => db.createObjectStore('s'));
    const v = { a: [1, 2], d: new Date(7) };
    const tx = db.transaction('s', 'readwrite');
    tx.objectStore('s').put(v, 'k');
    v.a.push(3);
    await txDone(tx);
    const got = await req(db.transaction('s').objectStore('s').get('k'));
    assert.deepEqual(got.a, [1, 2]);
    assert.ok(got.d instanceof Date && got.d.getTime() === 7);
    got.a.push(9);
    const again = await req(db.transaction('s').objectStore('s').get('k'));
    assert.deepEqual(again.a, [1, 2]);
    assert.throws(() => db.transaction('s', 'readwrite').objectStore('s').put(() => 1, 'f'), 'DataCloneError');
    db.close();
});

promise_test('auto-increment and in-line keys', async () => {
    const db = await openDb((db) => {
        db.createObjectStore('a', { autoIncrement: true });
        db.createObjectStore('b', { keyPath: 'id.n', autoIncrement: true });
    });
    const tx = db.transaction(['a', 'b'], 'readwrite');
    const a = tx.objectStore('a'), b = tx.objectStore('b');
    const k1 = await req(a.add('x'));
    const k2 = await req(a.add('y'));
    const k3 = await req(a.put('z', 10));
    const k4 = await req(a.add('w'));
    const kb = await req(b.add({ name: 'q' }));
    assert.deepEqual([k1, k2, k3, k4, kb], [1, 2, 10, 11, 1]);
    const vb = await req(b.get(1));
    assert.equal(vb.id.n, 1, 'the key is put into the value');
    assert.throws(() => b.put({ id: { n: 5 } }, 5), 'DataError');
    assert.throws(() => db.transaction('a', 'readwrite').objectStore('a').put('v', {}), 'DataError');
    db.close();
});

promise_test('add refuses an existing key, and the error aborts unless prevented', async () => {
    const db = await openDb((db) => db.createObjectStore('s'));
    let tx = db.transaction('s', 'readwrite');
    let s = tx.objectStore('s');
    s.add(1, 'k');
    const r = s.add(2, 'k');
    let name = '';
    r.onerror = (e) => { name = r.error.name; };
    await txDone(tx).then(() => assert.ok(false, 'committed'), (e) => assert.equal(e.name, 'ConstraintError'));
    assert.equal(name, 'ConstraintError');
    assert.equal(await req(db.transaction('s').objectStore('s').count()), 0, 'rolled back');
    tx = db.transaction('s', 'readwrite');
    s = tx.objectStore('s');
    s.add(1, 'k');
    s.add(2, 'k').onerror = (e) => e.preventDefault();
    s.add(3, 'j');
    assert.equal(await txDone(tx), 'complete');
    assert.equal(await req(db.transaction('s').objectStore('s').count()), 2);
    db.close();
});

promise_test('a unique index refuses a duplicate', async () => {
    const db = await openDb((db) => db.createObjectStore('s', { keyPath: 'id' }).createIndex('u', 'mail', { unique: true }));
    const tx = db.transaction('s', 'readwrite');
    const s = tx.objectStore('s');
    s.put({ id: 1, mail: 'a' });
    const r = s.put({ id: 2, mail: 'a' });
    r.onerror = (e) => e.preventDefault();
    await txDone(tx);
    assert.equal(r.error.name, 'ConstraintError');
    assert.equal(await req(db.transaction('s').objectStore('s').count()), 1);
    db.close();
});

promise_test('abort rolls back writes', async () => {
    const db = await openDb((db) => db.createObjectStore('s'));
    let tx = db.transaction('s', 'readwrite');
    tx.objectStore('s').put('a', 1);
    await txDone(tx);
    tx = db.transaction('s', 'readwrite');
    const s = tx.objectStore('s');
    s.put('b', 2);
    s.delete(1);
    await req(s.count());
    tx.abort();
    let aborted = false;
    tx.onabort = () => { aborted = true; };
    await new Promise((r) => setTimeout(r, 0));
    assert.ok(aborted);
    assert.equal(tx.error, null);
    const all = await req(db.transaction('s').objectStore('s').getAll());
    assert.deepEqual(all, ['a']);
    db.close();
});

promise_test('requests chain through promises inside one transaction', async () => {
    const db = await openDb((db) => db.createObjectStore('s'));
    const tx = db.transaction('s', 'readwrite');
    const s = tx.objectStore('s');
    await req(s.put(1, 'a'));
    const v = await req(s.get('a'));
    await req(s.put(v + 1, 'b'));
    assert.equal(await req(s.get('b')), 2);
    assert.equal(await txDone(tx), 'complete');
    assert.throws(() => s.get('a'), 'TransactionInactiveError');
    db.close();
});

promise_test('a transaction is inactive after its task', async () => {
    const db = await openDb((db) => db.createObjectStore('s'));
    const tx = db.transaction('s');
    const store = tx.objectStore('s');
    await new Promise((r) => setTimeout(r, 0));
    assert.throws(() => store.get(1), 'TransactionInactiveError');
    assert.throws(() => tx.objectStore('s'), 'InvalidStateError');
    assert.throws(() => db.transaction('s').objectStore('s').put(1, 1), 'ReadOnlyError');
    assert.throws(() => db.transaction('nope'), 'NotFoundError');
    db.close();
});

promise_test('cursors: directions, ranges, continue, advance, update and delete', async () => {
    const db = await openDb((db) => {
        const s = db.createObjectStore('s');
        s.createIndex('v', 'v');
    });
    let tx = db.transaction('s', 'readwrite');
    const s = tx.objectStore('s');
    for (let i = 1; i <= 6; i++) s.put({ v: i % 3 }, i);
    await txDone(tx);
    const walk = (src, range, dir, keyOnly) => new Promise((resolve) => {
        const out = [];
        const r = keyOnly ? src.openKeyCursor(range, dir) : src.openCursor(range, dir);
        r.onsuccess = () => {
            const c = r.result;
            if (!c) return resolve(out);
            out.push(keyOnly ? [c.key, c.primaryKey] : c.primaryKey);
            c.continue();
        };
    });
    const st = () => db.transaction('s').objectStore('s');
    assert.deepEqual(await walk(st(), null), [1, 2, 3, 4, 5, 6]);
    assert.deepEqual(await walk(st(), null, 'prev'), [6, 5, 4, 3, 2, 1]);
    assert.deepEqual(await walk(st(), IDBKeyRange.bound(2, 4)), [2, 3, 4]);
    assert.deepEqual(await walk(st(), IDBKeyRange.bound(2, 4, true, true), 'prev'), [3]);
    assert.deepEqual(await walk(st().index('v'), null, 'next', true), [[0, 3], [0, 6], [1, 1], [1, 4], [2, 2], [2, 5]]);
    assert.deepEqual(await walk(st().index('v'), null, 'nextunique', true), [[0, 3], [1, 1], [2, 2]]);
    assert.deepEqual(await walk(st().index('v'), null, 'prevunique', true), [[2, 2], [1, 1], [0, 3]]);
    assert.deepEqual(await walk(st().index('v'), 1, 'prev', true), [[1, 4], [1, 1]]);
    // continue(key) and advance(n)
    const jumps = await new Promise((resolve) => {
        const out = [];
        const r = st().openCursor();
        let n = 0;
        r.onsuccess = () => {
            const c = r.result;
            if (!c) return resolve(out);
            out.push(c.key);
            if (n++ === 0) c.continue(4); else c.advance(2);
        };
    });
    assert.deepEqual(jumps, [1, 4, 6]);
    // update and delete through a cursor
    tx = db.transaction('s', 'readwrite');
    await new Promise((resolve) => {
        const r = tx.objectStore('s').openCursor();
        r.onsuccess = () => {
            const c = r.result;
            if (!c) return resolve();
            assert.ok(c instanceof IDBCursorWithValue);
            if (c.key % 2) c.delete(); else c.update({ v: 10 + c.value.v });
            c.continue();
        };
    });
    await txDone(tx);
    assert.deepEqual(await req(st().getAll()), [{ v: 12 }, { v: 11 }, { v: 10 }]);
    assert.deepEqual(await req(st().index('v').getAllKeys(IDBKeyRange.lowerBound(11))), [4, 2]);
    db.close();
});

promise_test('multiEntry indexes', async () => {
    const db = await openDb((db) => db.createObjectStore('s', { keyPath: 'id' }).createIndex('tags', 'tags', { multiEntry: true }));
    const tx = db.transaction('s', 'readwrite');
    tx.objectStore('s').put({ id: 1, tags: ['a', 'b', 'a'] });
    tx.objectStore('s').put({ id: 2, tags: ['b'] });
    tx.objectStore('s').put({ id: 3, tags: 'c' });
    await txDone(tx);
    const ix = () => db.transaction('s').objectStore('s').index('tags');
    assert.deepEqual(await req(ix().getAllKeys('b')), [1, 2]);
    assert.deepEqual(await req(ix().getAllKeys('a')), [1]);
    assert.equal(await req(ix().count()), 4);
    db.close();
});

promise_test('events bubble from the request to the transaction and the database', async () => {
    const db = await openDb((db) => db.createObjectStore('s'));
    const seen = [];
    db.onerror = (e) => { seen.push('db ' + e.target.constructor.name); e.preventDefault(); };
    const tx = db.transaction('s', 'readwrite');
    tx.addEventListener('error', (e) => seen.push('tx ' + (e.target === r)));
    tx.objectStore('s').add(1, 1);
    const r = tx.objectStore('s').add(2, 1);
    await txDone(tx);
    assert.deepEqual(seen, ['tx true', 'db IDBRequest']);
    db.close();
});

promise_test('a later version upgrades; versionchange asks the old connection to close', async () => {
    const name = 'versions';
    const db1 = await req(Object.assign(indexedDB.open(name, 1), {
        onupgradeneeded(e) { e.target.result.createObjectStore('one'); },
    }));
    let asked = 0;
    db1.onversionchange = (e) => { asked = e.newVersion; db1.close(); };
    const r2 = indexedDB.open(name, 2);
    r2.onupgradeneeded = (e) => {
        assert.equal(e.oldVersion, 1);
        const db = r2.result;
        db.createObjectStore('two');
        db.deleteObjectStore('one');
    };
    const db2 = await req(r2);
    assert.equal(asked, 2);
    assert.deepEqual(Array.from(db2.objectStoreNames), ['two']);
    const dbs = await indexedDB.databases();
    assert.ok(dbs.some((d) => d.name === name && d.version === 2));
    const low = indexedDB.open(name, 1);
    await req(low).then(() => assert.ok(false), (e) => assert.equal(e.name, 'VersionError'));
    db2.close();
    const del = indexedDB.deleteDatabase(name);
    await req(del);
    assert.ok(!(await indexedDB.databases()).some((d) => d.name === name));
});

promise_test('an aborted upgrade leaves the database as it was', async () => {
    const r = indexedDB.open('aborted', 1);
    r.onupgradeneeded = () => { r.result.createObjectStore('x'); r.transaction.abort(); };
    await req(r).then(() => assert.ok(false), (e) => assert.equal(e.name, 'AbortError'));
    assert.ok(!(await indexedDB.databases()).some((d) => d.name === 'aborted'));
});

promise_test('a worker has databases of its own', async () => {
    const src = "const r=indexedDB.open('w',1);r.onupgradeneeded=()=>r.result.createObjectStore('s');" +
        "r.onsuccess=()=>{const t=r.result.transaction('s','readwrite');t.objectStore('s').put(5,'k');" +
        "t.oncomplete=()=>{r.result.transaction('s').objectStore('s').get('k').onsuccess=(e)=>" +
        "postMessage(e.target.result+' '+r.result.objectStoreNames.contains('s'))}}";
    const w = new Worker(URL.createObjectURL(new Blob([src])));
    const e = await once(w, 'message');
    assert.equal(e.data, '5 true');
    assert.ok(!(await indexedDB.databases()).some((d) => d.name === 'w'), 'not the document\'s');
});
