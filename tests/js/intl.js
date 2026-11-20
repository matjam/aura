// Intl (src/js/intl.js). The expected strings are Chrome's output for
// en-US. Every date test names its time zone; clock.js covers the local
// zone with a fixed clock.

const D = new Date(Date.UTC(2026, 10, 15, 15, 4, 5, 123)); // Sunday
const SUMMER = new Date(Date.UTC(2026, 6, 1, 15, 0, 0));
const NBSP = '\u00a0';
const fmt = (o, d) => new Intl.DateTimeFormat('en-US', Object.assign({ timeZone: 'UTC' }, o)).format(d || D);

test('Intl is a lazy global that behaves like a data property', () => {
    assert.equal(typeof Intl, 'object');
    assert.equal(Object.prototype.toString.call(Intl), '[object Intl]');
    const d = Object.getOwnPropertyDescriptor(globalThis, 'Intl');
    assert.equal(typeof d.value, 'object');
    assert.equal(d.enumerable, false);
    const saved = Intl;
    globalThis.Intl = 5;
    assert.equal(globalThis.Intl, 5);
    globalThis.Intl = saved;
    assert.ok(Intl.DateTimeFormat() instanceof Intl.DateTimeFormat, 'DateTimeFormat without new');
    assert.ok(Intl.NumberFormat() instanceof Intl.NumberFormat, 'NumberFormat without new');
    assert.ok(Intl.Collator() instanceof Intl.Collator, 'Collator without new');
    assert.throws(() => Intl.PluralRules(), 'TypeError');
    assert.equal(Object.prototype.toString.call(new Intl.NumberFormat()), '[object Intl.NumberFormat]');
});

test('locales', () => {
    assert.deepEqual(Intl.getCanonicalLocales(['EN-us', 'en-US']), ['en-US']);
    assert.deepEqual(Intl.getCanonicalLocales('zh-hant-tw'), ['zh-Hant-TW']);
    assert.deepEqual(Intl.getCanonicalLocales(), []);
    assert.throws(() => Intl.getCanonicalLocales('en_US'), 'RangeError');
    assert.throws(() => new Intl.NumberFormat('x'), 'RangeError');
    assert.deepEqual(Intl.DateTimeFormat.supportedLocalesOf(['en-GB', 'fr', 'en']), ['en-GB', 'en']);
    // every locale is formatted as en-US
    assert.equal(new Intl.NumberFormat('de-DE').format(1234.5), '1,234.5');
    assert.equal(new Intl.NumberFormat('de-DE').resolvedOptions().locale, 'en-US');
    assert.equal(new Intl.DateTimeFormat('default').resolvedOptions().locale, 'en-US');
    const l = new Intl.Locale('en-Latn-US-u-hc-h23');
    assert.equal(l.baseName, 'en-Latn-US');
    assert.equal(l.hourCycle, 'h23');
    assert.equal(l.region, 'US');
    assert.equal(new Intl.Locale('en').maximize().toString(), 'en-Latn-US');
    assert.equal(new Intl.Locale('en-Latn-US').minimize().toString(), 'en');
    assert.equal(new Intl.Locale('EN-gb').toString(), 'en-GB');
});

test('DateTimeFormat: defaults and styles', () => {
    assert.equal(fmt({}), '11/15/2026');
    assert.equal(fmt({ dateStyle: 'full' }), 'Sunday, November 15, 2026');
    assert.equal(fmt({ dateStyle: 'long' }), 'November 15, 2026');
    assert.equal(fmt({ dateStyle: 'medium' }), 'Nov 15, 2026');
    assert.equal(fmt({ dateStyle: 'short' }), '11/15/26');
    assert.equal(fmt({ timeStyle: 'full' }), '3:04:05 PM Coordinated Universal Time');
    assert.equal(fmt({ timeStyle: 'long' }), '3:04:05 PM UTC');
    assert.equal(fmt({ timeStyle: 'medium' }), '3:04:05 PM');
    assert.equal(fmt({ timeStyle: 'short' }), '3:04 PM');
    assert.equal(fmt({ dateStyle: 'medium', timeStyle: 'short' }), 'Nov 15, 2026, 3:04 PM');
    assert.equal(fmt({ dateStyle: 'long', timeStyle: 'short' }), 'November 15, 2026 at 3:04 PM');
    assert.equal(fmt({ dateStyle: 'short', timeStyle: 'short' }), '11/15/26, 3:04 PM');
});

test('DateTimeFormat: components', () => {
    assert.equal(fmt({ weekday: 'long', year: 'numeric', month: 'long', day: 'numeric' }), 'Sunday, November 15, 2026');
    assert.equal(fmt({ weekday: 'short', year: 'numeric', month: 'short', day: 'numeric' }), 'Sun, Nov 15, 2026');
    assert.equal(fmt({ month: 'short', day: 'numeric' }), 'Nov 15');
    assert.equal(fmt({ year: 'numeric', month: 'long' }), 'November 2026');
    assert.equal(fmt({ year: 'numeric', month: 'short' }), 'Nov 2026');
    assert.equal(fmt({ month: '2-digit', day: '2-digit', year: 'numeric' }), '11/15/2026');
    assert.equal(fmt({ month: 'numeric', year: 'numeric' }), '11/2026');
    assert.equal(fmt({ year: 'numeric' }), '2026');
    assert.equal(fmt({ month: 'long' }), 'November');
    assert.equal(fmt({ weekday: 'long' }), 'Sunday');
    assert.equal(fmt({ year: 'numeric', era: 'short' }), '2026 AD');
    assert.equal(fmt({ hour: 'numeric' }), '3 PM');
    assert.equal(fmt({ hour: 'numeric', minute: 'numeric' }), '3:04 PM');
    assert.equal(fmt({ hour: '2-digit', minute: '2-digit' }), '03:04 PM');
    assert.equal(fmt({ hour: 'numeric', minute: '2-digit', hour12: false }), '15:04');
    assert.equal(fmt({ hour: 'numeric', minute: '2-digit', hourCycle: 'h23' }, new Date(Date.UTC(2026, 0, 1, 3, 4))), '03:04');
    assert.equal(fmt({ minute: '2-digit', second: '2-digit', fractionalSecondDigits: 3 }), '04:05.123');
    assert.equal(fmt({ year: 'numeric', month: 'short', day: 'numeric', hour: 'numeric', minute: 'numeric', timeZoneName: 'short' }),
                 'Nov 15, 2026, 3:04 PM UTC');
    assert.equal(fmt({ year: 'numeric', month: 'long', day: 'numeric', hour: 'numeric', minute: 'numeric' }),
                 'November 15, 2026 at 3:04 PM');
    assert.equal(fmt({ timeZoneName: 'short' }), '11/15/2026, UTC');
});

test('DateTimeFormat: time zones', () => {
    assert.equal(fmt({ timeStyle: 'long', timeZone: 'America/New_York' }), '10:04:05 AM EST');
    assert.equal(fmt({ timeStyle: 'long', timeZone: 'America/New_York' }, SUMMER), '11:00:00 AM EDT');
    assert.equal(fmt({ timeStyle: 'full', timeZone: 'America/Los_Angeles' }), '7:04:05 AM Pacific Standard Time');
    assert.equal(fmt({ timeStyle: 'long', timeZone: 'Europe/London' }, SUMMER), '4:00:00 PM GMT+1');
    assert.equal(fmt({ timeStyle: 'long', timeZone: 'Europe/London' }), '3:04:05 PM GMT');
    assert.equal(fmt({ timeStyle: 'long', timeZone: 'Europe/Berlin' }), '4:04:05 PM GMT+1');
    assert.equal(fmt({ timeStyle: 'long', timeZone: 'Asia/Kolkata' }), '8:34:05 PM GMT+5:30');
    assert.equal(fmt({ timeStyle: 'long', timeZone: 'Australia/Sydney' }), '2:04:05 AM GMT+11');
    assert.equal(fmt({ timeZoneName: 'longOffset', hour: 'numeric', timeZone: 'Asia/Kolkata' }), '8 PM GMT+05:30');
    assert.equal(new Intl.DateTimeFormat('en-US', { timeZone: 'etc/utc' }).resolvedOptions().timeZone, 'UTC');
    assert.equal(new Intl.DateTimeFormat('en-US', { timeZone: 'america/new_york' }).resolvedOptions().timeZone,
                 'America/New_York');
    assert.throws(() => new Intl.DateTimeFormat('en-US', { timeZone: 'Mars/Olympus_Mons' }), 'RangeError');
});

test('DateTimeFormat: formatToParts, resolvedOptions, formatRange', () => {
    const f = new Intl.DateTimeFormat('en-US', { year: 'numeric', month: 'short', day: 'numeric', hour: 'numeric',
                                                 minute: 'numeric', timeZone: 'UTC' });
    assert.deepEqual(f.formatToParts(D), [
        { type: 'month', value: 'Nov' }, { type: 'literal', value: ' ' }, { type: 'day', value: '15' },
        { type: 'literal', value: ', ' }, { type: 'year', value: '2026' }, { type: 'literal', value: ', ' },
        { type: 'hour', value: '3' }, { type: 'literal', value: ':' }, { type: 'minute', value: '04' },
        { type: 'literal', value: ' ' }, { type: 'dayPeriod', value: 'PM' }]);
    assert.deepEqual(f.resolvedOptions(), { locale: 'en-US', calendar: 'gregory', numberingSystem: 'latn',
        timeZone: 'UTC', hourCycle: 'h12', hour12: true, year: 'numeric', month: 'short', day: 'numeric',
        hour: 'numeric', minute: '2-digit' });
    assert.deepEqual(new Intl.DateTimeFormat('en-US', { timeZone: 'UTC' }).resolvedOptions(), { locale: 'en-US',
        calendar: 'gregory', numberingSystem: 'latn', timeZone: 'UTC', year: 'numeric', month: 'numeric', day: 'numeric' });
    assert.deepEqual(new Intl.DateTimeFormat('en-US', { dateStyle: 'medium', timeStyle: 'short', timeZone: 'UTC' }).resolvedOptions(),
        { locale: 'en-US', calendar: 'gregory', numberingSystem: 'latn', timeZone: 'UTC', hourCycle: 'h12', hour12: true,
          dateStyle: 'medium', timeStyle: 'short' });
    assert.equal(new Intl.DateTimeFormat([], { hour: 'numeric' }).resolvedOptions().hour12, true);
    assert.equal(new Intl.DateTimeFormat('en-US', { hour: 'numeric', hour12: false }).resolvedOptions().hourCycle, 'h23');
    assert.deepEqual([D].map(f.format), ['Nov 15, 2026, 3:04 PM']);
    const g = new Intl.DateTimeFormat('en-US', { timeZone: 'UTC' });
    assert.equal(g.formatRange(D, D), '11/15/2026');
    assert.equal(g.formatRange(D, SUMMER), '11/15/2026\u2009\u2013\u20097/1/2026');
    assert.equal(g.formatRangeToParts(D, SUMMER)[0].source, 'startRange');
});

test('DateTimeFormat: errors', () => {
    assert.throws(() => fmt({}, new Date(NaN)), 'RangeError');
    assert.throws(() => new Intl.DateTimeFormat('en-US', { dateStyle: 'long', year: 'numeric' }), 'TypeError');
    assert.throws(() => new Intl.DateTimeFormat('en-US', { month: 'tiny' }), 'RangeError');
    assert.throws(() => D.toLocaleTimeString('en-US', { dateStyle: 'long' }), 'TypeError');
});

test('Date locale methods route through Intl', () => {
    assert.equal(D.toLocaleString('en-US', { timeZone: 'UTC' }), '11/15/2026, 3:04:05 PM');
    assert.equal(D.toLocaleDateString('en-US', { timeZone: 'UTC' }), '11/15/2026');
    assert.equal(D.toLocaleTimeString('en-US', { timeZone: 'UTC' }), '3:04:05 PM');
    assert.equal(D.toLocaleDateString(undefined, { month: 'long', day: 'numeric', timeZone: 'UTC' }), 'November 15');
    assert.equal(D.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', timeZone: 'UTC' }), '03:04 PM');
    assert.equal(new Date(NaN).toLocaleString(), 'Invalid Date');
    assert.throws(() => Date.prototype.toLocaleString.call({}), 'TypeError');
});

test('NumberFormat: decimal and rounding', () => {
    const n = (x, o) => new Intl.NumberFormat('en-US', o).format(x);
    assert.equal(n(1234567.891), '1,234,567.891');
    assert.equal(n(1234.5678), '1,234.568');
    assert.equal(n(0), '0');
    assert.equal(n(-0), '-0');
    assert.equal(n(-1234.5), '-1,234.5');
    assert.equal(n(1.005, { maximumFractionDigits: 2 }), '1.01');
    assert.equal(n(2.5, { maximumFractionDigits: 0 }), '3');
    assert.equal(n(2.5, { maximumFractionDigits: 0, roundingMode: 'halfEven' }), '2');
    assert.equal(n(5, { minimumFractionDigits: 2 }), '5.00');
    assert.equal(n(123456, { maximumSignificantDigits: 3 }), '123,000');
    assert.equal(n(0.000123456, { maximumSignificantDigits: 2 }), '0.00012');
    assert.equal(n(1, { minimumSignificantDigits: 3 }), '1.00');
    assert.equal(n(5, { minimumIntegerDigits: 3 }), '005');
    assert.equal(n(12345, { useGrouping: false }), '12345');
    assert.equal(n(1234, { useGrouping: 'min2' }), '1234');
    assert.equal(n(12345, { useGrouping: 'min2' }), '12,345');
    assert.equal(n(5, { signDisplay: 'always' }), '+5');
    assert.equal(n(0, { signDisplay: 'exceptZero' }), '0');
    assert.equal(n(-5, { signDisplay: 'never' }), '5');
    assert.equal(n(1.5, { maximumFractionDigits: 2, trailingZeroDisplay: 'stripIfInteger', minimumFractionDigits: 2 }), '1.50');
    assert.equal(n(1, { minimumFractionDigits: 2, trailingZeroDisplay: 'stripIfInteger' }), '1');
    assert.equal(n(NaN), 'NaN');
    assert.equal(n(-Infinity), '-∞');
    assert.equal(n(1234, { notation: 'scientific' }), '1.234E3');
    assert.equal(n(0.00012, { notation: 'scientific' }), '1.2E-4');
    assert.equal(n(12345, { notation: 'engineering' }), '12.345E3');
    assert.equal(n(12345678901234567890n), '12,345,678,901,234,567,890');
    assert.equal(n('1234567890123456789.5'), '1,234,567,890,123,456,789.5');
    assert.equal((1234.5).toLocaleString(), '1,234.5');
    assert.equal((0.5).toLocaleString('en-US', { style: 'percent' }), '50%');
    assert.equal(12345678901234567890n.toLocaleString(), '12,345,678,901,234,567,890');
    assert.throws(() => Number.prototype.toLocaleString.call('1'), 'TypeError');
    assert.throws(() => n(1, { maximumFractionDigits: 101 }), 'RangeError');
    assert.throws(() => n(1, { minimumFractionDigits: 3, maximumFractionDigits: 1 }), 'RangeError');
});

test('NumberFormat: percent, currency, unit', () => {
    const n = (x, o) => new Intl.NumberFormat('en-US', o).format(x);
    assert.equal(n(0.256, { style: 'percent' }), '26%');
    assert.equal(n(0.07, { style: 'percent', minimumFractionDigits: 1 }), '7.0%');
    const usd = { style: 'currency', currency: 'USD' };
    assert.equal(n(1234.5, usd), '$1,234.50');
    assert.equal(n(-5, usd), '-$5.00');
    assert.equal(n(-5, Object.assign({ currencySign: 'accounting' }, usd)), '($5.00)');
    assert.equal(n(1234.5, Object.assign({ currencyDisplay: 'code' }, usd)), 'USD' + NBSP + '1,234.50');
    assert.equal(n(1234.5, Object.assign({ currencyDisplay: 'name' }, usd)), '1,234.50 US dollars');
    assert.equal(n(1, Object.assign({ currencyDisplay: 'name' }, usd)), '1.00 US dollars');
    assert.equal(n(1234.5, { style: 'currency', currency: 'EUR' }), '€1,234.50');
    assert.equal(n(1234.5, { style: 'currency', currency: 'jpy' }), '¥1,235');
    assert.equal(n(1234.5, { style: 'currency', currency: 'CHF' }), 'CHF' + NBSP + '1,234.50');
    assert.equal(n(5, { style: 'currency', currency: 'CAD' }), 'CA$5.00');
    assert.equal(n(5, { style: 'currency', currency: 'CAD', currencyDisplay: 'narrowSymbol' }), '$5.00');
    assert.equal(n(1500, { style: 'currency', currency: 'USD', notation: 'compact' }), '$1.5K');
    assert.throws(() => n(1, { style: 'currency' }), 'TypeError');
    assert.throws(() => n(1, { style: 'currency', currency: 'US' }), 'RangeError');
    const u = (x, unit, unitDisplay) => n(x, { style: 'unit', unit, unitDisplay });
    assert.equal(u(512, 'kilobyte'), '512 kB');
    assert.equal(u(512, 'kilobyte', 'long'), '512 kilobytes');
    assert.equal(u(1, 'kilobyte', 'long'), '1 kilobyte');
    assert.equal(u(512, 'kilobyte', 'narrow'), '512kB');
    assert.equal(u(3, 'day'), '3 days');
    assert.equal(u(1, 'day'), '1 day');
    assert.equal(u(3, 'hour'), '3 hr');
    assert.equal(u(3, 'second'), '3 sec');
    assert.equal(u(3, 'year'), '3 yrs');
    assert.equal(u(3, 'month', 'narrow'), '3m');
    assert.equal(u(5, 'percent'), '5%');
    assert.equal(u(21, 'celsius'), '21°C');
    assert.equal(u(60, 'mile-per-hour'), '60 mph');
    assert.equal(u(60, 'kilometer-per-hour', 'long'), '60 kilometers per hour');
    assert.equal(u(6, 'foot', 'long'), '6 feet');
    assert.throws(() => u(1, 'furlong'), 'RangeError');
});

test('NumberFormat: compact notation', () => {
    const c = (x, o) => new Intl.NumberFormat('en-US', Object.assign({ notation: 'compact' }, o)).format(x);
    assert.equal(c(999), '999');
    assert.equal(c(1234), '1.2K');
    assert.equal(c(1500), '1.5K');
    assert.equal(c(12345), '12K');
    assert.equal(c(123456), '123K');
    assert.equal(c(999999), '1M');
    assert.equal(c(1234567), '1.2M');
    assert.equal(c(1.5e9), '1.5B');
    assert.equal(c(2e12), '2T');
    assert.equal(c(-1234), '-1.2K');
    assert.equal(c(1.234), '1.2');
    assert.equal(c(0.1234), '0.12');
    assert.equal(c(1234, { compactDisplay: 'long' }), '1.2 thousand');
    assert.equal(c(5e6, { compactDisplay: 'long' }), '5 million');
    // GitHub's "Used by" count
    assert.equal(c(1234, { maximumFractionDigits: 1, maximumSignificantDigits: 2 }), '1.2K');
    assert.equal(c(12345678, { maximumFractionDigits: 1, maximumSignificantDigits: 2 }), '12M');
});

test('NumberFormat: formatToParts and resolvedOptions', () => {
    const f = new Intl.NumberFormat('en-US');
    assert.deepEqual(f.formatToParts(-1234.5), [
        { type: 'minusSign', value: '-' }, { type: 'integer', value: '1' }, { type: 'group', value: ',' },
        { type: 'integer', value: '234' }, { type: 'decimal', value: '.' }, { type: 'fraction', value: '5' }]);
    assert.deepEqual(new Intl.NumberFormat('en-US', { style: 'currency', currency: 'USD' }).formatToParts(5), [
        { type: 'currency', value: '$' }, { type: 'integer', value: '5' }, { type: 'decimal', value: '.' },
        { type: 'fraction', value: '00' }]);
    assert.deepEqual(new Intl.NumberFormat('en-US', { notation: 'compact' }).formatToParts(1500), [
        { type: 'integer', value: '1' }, { type: 'decimal', value: '.' }, { type: 'fraction', value: '5' },
        { type: 'compact', value: 'K' }]);
    assert.deepEqual(new Intl.NumberFormat('en-US', { style: 'unit', unit: 'megabyte' }).formatToParts(3), [
        { type: 'integer', value: '3' }, { type: 'literal', value: ' ' }, { type: 'unit', value: 'MB' }]);
    assert.deepEqual(new Intl.NumberFormat('en-US', { style: 'percent' }).formatToParts(0.5), [
        { type: 'integer', value: '50' }, { type: 'percentSign', value: '%' }]);
    assert.deepEqual(f.resolvedOptions(), { locale: 'en-US', numberingSystem: 'latn', style: 'decimal',
        minimumIntegerDigits: 1, minimumFractionDigits: 0, maximumFractionDigits: 3, useGrouping: 'auto',
        notation: 'standard', signDisplay: 'auto', roundingIncrement: 1, roundingMode: 'halfExpand',
        roundingPriority: 'auto', trailingZeroDisplay: 'auto' });
    assert.deepEqual(new Intl.NumberFormat('en-US', { style: 'currency', currency: 'eur' }).resolvedOptions(), {
        locale: 'en-US', numberingSystem: 'latn', style: 'currency', currency: 'EUR', currencyDisplay: 'symbol',
        currencySign: 'standard', minimumIntegerDigits: 1, minimumFractionDigits: 2, maximumFractionDigits: 2,
        useGrouping: 'auto', notation: 'standard', signDisplay: 'auto', roundingIncrement: 1,
        roundingMode: 'halfExpand', roundingPriority: 'auto', trailingZeroDisplay: 'auto' });
    assert.deepEqual(new Intl.NumberFormat('en-US', { notation: 'compact' }).resolvedOptions(), {
        locale: 'en-US', numberingSystem: 'latn', style: 'decimal', minimumIntegerDigits: 1,
        minimumFractionDigits: 0, maximumFractionDigits: 0, minimumSignificantDigits: 1, maximumSignificantDigits: 2,
        useGrouping: 'min2', notation: 'compact', compactDisplay: 'short', signDisplay: 'auto', roundingIncrement: 1,
        roundingMode: 'halfExpand', roundingPriority: 'morePrecision', trailingZeroDisplay: 'auto' });
    assert.deepEqual(['1', '22'].map(f.format), ['1', '22']);
});

test('RelativeTimeFormat', () => {
    const r = new Intl.RelativeTimeFormat('en');
    assert.equal(r.format(-1, 'day'), '1 day ago');
    assert.equal(r.format(3, 'days'), 'in 3 days');
    assert.equal(r.format(0, 'day'), 'in 0 days');
    assert.equal(r.format(-0, 'day'), '0 days ago');
    assert.equal(r.format(1.5, 'hour'), 'in 1.5 hours');
    assert.equal(r.format(-1000, 'year'), '1,000 years ago');
    const a = new Intl.RelativeTimeFormat('en', { numeric: 'auto' });
    assert.equal(a.format(-1, 'day'), 'yesterday');
    assert.equal(a.format(0, 'day'), 'today');
    assert.equal(a.format(1, 'day'), 'tomorrow');
    assert.equal(a.format(0, 'second'), 'now');
    assert.equal(a.format(-1, 'year'), 'last year');
    assert.equal(a.format(1, 'month'), 'next month');
    assert.equal(a.format(0, 'week'), 'this week');
    assert.equal(a.format(-2, 'day'), '2 days ago');
    assert.equal(a.format(-1, 'hour'), '1 hour ago');
    const s = new Intl.RelativeTimeFormat('en', { style: 'short' });
    assert.equal(s.format(-3, 'hour'), '3 hr. ago');
    assert.equal(s.format(2, 'month'), 'in 2 mo.');
    assert.equal(s.format(-5, 'minute'), '5 min. ago');
    assert.equal(s.format(-2, 'day'), '2 days ago');
    assert.equal(new Intl.RelativeTimeFormat('en', { style: 'short', numeric: 'auto' }).format(-1, 'year'), 'last yr.');
    assert.equal(new Intl.RelativeTimeFormat('en', { style: 'narrow' }).format(-5, 'minute'), '5m ago');
    assert.deepEqual(r.formatToParts(100, 'day'), [
        { type: 'literal', value: 'in ' }, { type: 'integer', value: '100', unit: 'day' },
        { type: 'literal', value: ' days' }]);
    assert.deepEqual(r.formatToParts(-1234, 'second'), [
        { type: 'integer', value: '1', unit: 'second' }, { type: 'group', value: ',', unit: 'second' },
        { type: 'integer', value: '234', unit: 'second' }, { type: 'literal', value: ' seconds ago' }]);
    assert.deepEqual(a.formatToParts(-1, 'day'), [{ type: 'literal', value: 'yesterday' }]);
    assert.deepEqual(r.resolvedOptions(), { locale: 'en-US', style: 'long', numeric: 'always', numberingSystem: 'latn' });
    assert.throws(() => r.format(1, 'fortnight'), 'RangeError');
    assert.throws(() => r.format(Infinity, 'day'), 'RangeError');
});

test('PluralRules', () => {
    const p = new Intl.PluralRules('en-US');
    assert.deepEqual([0, 1, 2, 1.5, -1].map((n) => p.select(n)), ['other', 'one', 'other', 'other', 'one']);
    assert.equal(new Intl.PluralRules('en-US', { minimumFractionDigits: 1 }).select(1), 'other');
    const o = new Intl.PluralRules('en-US', { type: 'ordinal' });
    assert.deepEqual([1, 2, 3, 4, 11, 12, 13, 21, 22, 23, 101, 111].map((n) => o.select(n)),
                     ['one', 'two', 'few', 'other', 'other', 'other', 'other', 'one', 'two', 'few', 'one', 'other']);
    assert.deepEqual(p.resolvedOptions(), { locale: 'en-US', type: 'cardinal', minimumIntegerDigits: 1,
        minimumFractionDigits: 0, maximumFractionDigits: 3, pluralCategories: ['one', 'other'], roundingIncrement: 1,
        roundingMode: 'halfExpand', roundingPriority: 'auto', trailingZeroDisplay: 'auto' });
    assert.deepEqual(o.resolvedOptions().pluralCategories, ['few', 'one', 'two', 'other']);
});

test('Collator and localeCompare', () => {
    const c = new Intl.Collator('en');
    assert.deepEqual(['b', 'A', 'a', 'B', 'ä', 'Z', '10', '9', ' x', '_y'].sort(c.compare),
                     [' x', '_y', '10', '9', 'a', 'A', 'ä', 'b', 'B', 'Z']);
    assert.deepEqual(['a10', 'a2', 'a1'].sort(new Intl.Collator('en', { numeric: true }).compare), ['a1', 'a2', 'a10']);
    assert.equal(new Intl.Collator('en', { sensitivity: 'base' }).compare('a', 'A'), 0);
    assert.equal(new Intl.Collator('en', { sensitivity: 'base' }).compare('a', 'á'), 0);
    assert.equal(new Intl.Collator('en', { sensitivity: 'accent' }).compare('a', 'á'), -1);
    assert.equal(new Intl.Collator('en', { sensitivity: 'accent' }).compare('a', 'A'), 0);
    assert.equal(c.compare('résumé', 'resume'), 1);
    assert.equal(c.compare('a', 'a'), 0);
    assert.equal(new Intl.Collator('en', { caseFirst: 'upper' }).compare('a', 'A'), 1);
    assert.equal(new Intl.Collator('en', { ignorePunctuation: true }).compare('a-b', 'ab'), 0);
    assert.equal('a'.localeCompare('b'), -1);
    assert.equal('a'.localeCompare('B'), -1);
    assert.equal('b'.localeCompare('a'), 1);
    assert.equal('ä'.localeCompare('z', 'de'), -1);
    assert.equal('a'.localeCompare('A', undefined, { sensitivity: 'base' }), 0);
    assert.equal('item10'.localeCompare('item9', undefined, { numeric: true }), 1);
    assert.equal(String.prototype.localeCompare.length, 1);
    assert.equal(Number.prototype.toLocaleString.length, 0);
    assert.equal(Date.prototype.toLocaleDateString.length, 0);
    assert.deepEqual(c.resolvedOptions(), { locale: 'en-US', usage: 'sort', sensitivity: 'variant',
        ignorePunctuation: false, collation: 'default', numeric: false, caseFirst: 'false' });
});

test('ListFormat', () => {
    const l = (list, o) => new Intl.ListFormat('en', o).format(list);
    assert.equal(l([]), '');
    assert.equal(l(['a']), 'a');
    assert.equal(l(['a', 'b']), 'a and b');
    assert.equal(l(['a', 'b', 'c']), 'a, b, and c');
    assert.equal(l(['a', 'b', 'c'], { style: 'short' }), 'a, b, & c');
    assert.equal(l(['a', 'b', 'c'], { style: 'narrow' }), 'a, b, c');
    assert.equal(l(['a', 'b'], { type: 'disjunction' }), 'a or b');
    assert.equal(l(['a', 'b', 'c'], { type: 'disjunction' }), 'a, b, or c');
    assert.equal(l(['1 hr', '5 min'], { type: 'unit' }), '1 hr, 5 min');
    assert.equal(l(['1h', '5m'], { type: 'unit', style: 'narrow' }), '1h 5m');
    assert.deepEqual(new Intl.ListFormat('en').formatToParts(['x', 'y']), [
        { type: 'element', value: 'x' }, { type: 'literal', value: ' and ' }, { type: 'element', value: 'y' }]);
    assert.throws(() => l([1, 2]), 'TypeError');
    assert.deepEqual(new Intl.ListFormat('en').resolvedOptions(), { locale: 'en-US', type: 'conjunction', style: 'long' });
});

test('Segmenter', () => {
    const g = new Intl.Segmenter('en');
    const e = 'e\u0301';
    assert.deepEqual([...g.segment(e + 'a')].map((s) => s.segment), [e, 'a']);
    const family = '\u{1F468}\u200D\u{1F469}\u200D\u{1F467}';
    const flag = '\u{1F1FA}\u{1F1F8}';
    assert.deepEqual([...g.segment('a' + family + flag + '\r\nb')].map((s) => s.segment), ['a', family, flag, '\r\n', 'b']);
    const w = new Intl.Segmenter('en', { granularity: 'word' });
    const segs = [...w.segment("Hello, world! It's 3.14")];
    assert.deepEqual(segs.map((s) => s.segment), ['Hello', ',', ' ', 'world', '!', ' ', "It's", ' ', '3.14']);
    assert.deepEqual(segs.map((s) => s.isWordLike), [true, false, false, true, false, false, true, false, true]);
    assert.deepEqual(segs[3], { segment: 'world', index: 7, input: "Hello, world! It's 3.14", isWordLike: true });
    assert.equal(w.segment('Hello world').containing(8).segment, 'world');
    assert.equal(w.segment('Hello world').containing(5).segment, ' ');
    assert.equal(w.segment('Hello').containing(9), undefined);
    const s = new Intl.Segmenter('en', { granularity: 'sentence' });
    assert.deepEqual([...s.segment('One. Two? Three')].map((x) => x.segment), ['One. ', 'Two? ', 'Three']);
    assert.deepEqual(g.resolvedOptions(), { locale: 'en-US', granularity: 'grapheme' });
});

test('DisplayNames', () => {
    assert.equal(new Intl.DisplayNames(['en'], { type: 'region' }).of('US'), 'United States');
    assert.equal(new Intl.DisplayNames(['en'], { type: 'language' }).of('fr'), 'French');
    assert.equal(new Intl.DisplayNames(['en'], { type: 'language' }).of('en-US'), 'American English');
    assert.equal(new Intl.DisplayNames(['en'], { type: 'currency' }).of('USD'), 'US Dollar');
    assert.equal(new Intl.DisplayNames(['en'], { type: 'region' }).of('XX'), 'XX');
    assert.equal(new Intl.DisplayNames(['en'], { type: 'region', fallback: 'none' }).of('XX'), undefined);
    assert.throws(() => new Intl.DisplayNames('en'), 'TypeError');
});

test('the local zone is one that keeps the clock\'s time, and Date names it', () => {
    const id = Intl.DateTimeFormat().resolvedOptions().timeZone;
    const o = { hour: 'numeric', minute: 'numeric', day: 'numeric', hourCycle: 'h23' };
    for (const month of [0, 6]) {
        const d = new Date(Date.UTC(2026, month, 15, 12));
        assert.equal(new Intl.DateTimeFormat('en-US', o).format(d),
            new Intl.DateTimeFormat('en-US', Object.assign({ timeZone: id }, o)).format(d), id + ' in month ' + month);
    }
    const y = new Date().getFullYear();
    const dst = new Date(y, 0, 15).getTimezoneOffset() !== new Date(y, 6, 15).getTimezoneOffset();
    if (dst) assert.ok(!/^Etc\//.test(id), 'a clock with daylight time is not in a zone without it: ' + id);
    const long = new Intl.DateTimeFormat('en-US', { timeZoneName: 'long' }).formatToParts(new Date())
        .find((p) => p.type === 'timeZoneName').value;
    const s = String(new Date());
    assert.ok(/GMT[+-]\d{4}/.test(s), s);
    assert.equal(/\(.+\)$/.test(s), !/^GMT/.test(long), s + ' / ' + long);
    if (!/^GMT/.test(long)) assert.ok(s.endsWith('(' + long + ')') && new Date().toTimeString().endsWith('(' + long + ')'));
    assert.equal(Date.prototype.toString.name, 'toString');
    assert.equal(String(new Date(NaN)), 'Invalid Date');
});
