// VisualDay calendar feed.
//
// Deployed as a web app from your own Google account, this returns the events
// between two times as compact JSON for the Paper Color to draw. Google expands
// recurring events for us, so the device never has to.
//
// Request:  <web app url>?token=<TOKEN>&start=<unix seconds>&end=<unix seconds>
// Response: {"events":[{"t":title,"s":start,"e":end,"a":allDay,"l":location}],
//            "bins":[{"c":"green"|"brown"|"black","d":"YYYY-MM-DD"}]}
//
// Bins come from Google Tasks named "Green Bin", "Brown Bin" or "Black Bin"
// that are due in the range and not yet ticked off. This needs the Tasks
// advanced service (Services > Tasks API in the editor).
//
// Setup is in apps-script/README.md.

// Calendars to include. Empty means your default (main) calendar. To add more,
// list their IDs (Calendar settings > Integrate calendar > Calendar ID), e.g.
// ['primary', 'family12345@group.calendar.google.com'].
const CALENDAR_IDS = [];

const MAX_EVENTS = 60;
const MAX_TITLE = 80;
const MAX_LOCATION = 60;

function doGet(e) {
  const p = (e && e.parameter) || {};
  const token = PropertiesService.getScriptProperties().getProperty('TOKEN');
  if (!token || p.token !== token) return json({ error: 'bad token' });

  const now = Math.floor(Date.now() / 1000);
  const start = parseInt(p.start, 10) || now - 12 * 3600;
  const end = parseInt(p.end, 10) || start + 2 * 86400;
  if (end <= start || end - start > 7 * 86400) return json({ error: 'bad range' });

  const from = new Date(start * 1000);
  const to = new Date(end * 1000);
  const events = [];
  calendars().forEach(function (cal) {
    cal.getEvents(from, to).forEach(function (ev) {
      if (ev.getMyStatus() === CalendarApp.GuestStatus.NO) return; // declined
      events.push({
        t: ascii(ev.getTitle(), MAX_TITLE) || '(no title)',
        s: Math.floor(ev.getStartTime().getTime() / 1000),
        e: Math.floor(ev.getEndTime().getTime() / 1000),
        a: ev.isAllDayEvent() ? 1 : 0,
        l: ascii(ev.getLocation(), MAX_LOCATION),
      });
    });
  });
  events.sort(function (x, y) { return x.s - y.s || x.e - y.e; });
  return json({ events: events.slice(0, MAX_EVENTS), bins: bins(from, to) });
}

// Run this once from the editor (select it, press Run). It creates the secret
// token if there isn't one and logs it; append "?token=..." to your web app URL.
function setup() {
  const props = PropertiesService.getScriptProperties();
  let token = props.getProperty('TOKEN');
  if (!token) {
    token = Utilities.getUuid().replace(/-/g, '');
    props.setProperty('TOKEN', token);
  }
  calendars(); // touches the calendar and tasks so authorisation is requested now
  const now = new Date();
  Logger.log('Bins due today or tomorrow: ' + JSON.stringify(bins(now, new Date(now.getTime() + 86400000))));
  Logger.log('Token: ' + token);
  Logger.log('Device URL: <your web app URL>?token=' + token);
}

// Open bin tasks due between two times. Task due dates are whole days, so the
// device matches them on the date alone.
function bins(from, to) {
  if (typeof Tasks === 'undefined') return []; // advanced service not enabled
  const dayMs = 86400000;
  const out = [];
  (Tasks.Tasklists.list({ maxResults: 100 }).items || []).forEach(function (list) {
    let pageToken;
    do {
      const page = Tasks.Tasks.list(list.id, {
        showCompleted: false,
        dueMin: new Date(from.getTime() - dayMs).toISOString(),
        dueMax: new Date(to.getTime() + dayMs).toISOString(),
        maxResults: 100,
        pageToken: pageToken,
      });
      (page.items || []).forEach(function (t) {
        const m = /^\s*(green|brown|black)\s+bins?\s*$/i.exec(t.title || '');
        if (m && t.due) out.push({ c: m[1].toLowerCase(), d: t.due.slice(0, 10) });
      });
      pageToken = page.nextPageToken;
    } while (pageToken);
  });
  return out;
}

function calendars() {
  if (!CALENDAR_IDS.length) return [CalendarApp.getDefaultCalendar()];
  return CALENDAR_IDS.map(function (id) {
    return id === 'primary' ? CalendarApp.getDefaultCalendar() : CalendarApp.getCalendarById(id);
  }).filter(function (c) { return c; });
}

// The device's fonts are plain ASCII: map typographic punctuation, strip
// accents, and drop anything else (emoji and so on).
function ascii(s, max) {
  s = String(s || '')
    .replace(/[‘’‚′]/g, "'")
    .replace(/[“”„″]/g, '"')
    .replace(/[–—−]/g, '-')
    .replace(/…/g, '...')
    .replace(/[   \n\r\t]/g, ' ')
    .normalize('NFD').replace(/[̀-ͯ]/g, '')
    .replace(/[^\x20-\x7E]/g, '')
    .replace(/\s+/g, ' ')
    .trim();
  return s.length > max ? s.slice(0, max - 3).trim() + '...' : s;
}

function json(obj) {
  return ContentService.createTextOutput(JSON.stringify(obj))
    .setMimeType(ContentService.MimeType.JSON);
}
