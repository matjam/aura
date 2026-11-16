/* Tests for the list windows' models (src/browser/lists.c): the rows of
 * History (grouped by day), Bookmarks (folders and depth), Downloads
 * (states) and Cookies, each action's enable rule and effect, the
 * callbacks for actions that ask, and the change notice. */
#include "browser/listview.h"
#include "browser/lists.h"
#include "url/url.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond)                                                                                               \
    do {                                                                                                          \
        checks++;                                                                                                 \
        if (!(cond)) {                                                                                            \
            failures++;                                                                                           \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
        }                                                                                                         \
    } while (0)

#define CHECK_STR(a, b)                                                                                           \
    do {                                                                                                          \
        const char *a_ = (a), *b_ = (b);                                                                          \
        checks++;                                                                                                 \
        if (!a_ || strcmp(a_, b_) != 0) {                                                                         \
            failures++;                                                                                           \
            printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, a_ ? a_ : "(null)", b_);                \
        }                                                                                                         \
    } while (0)

/* ---- A host that records what it was asked ---- */

typedef struct Rec {
    char url[256];
    bool new_tab;
    int opens;
    const char *answer; /* ask_text's reply; NULL cancels */
    char prompt[128], initial[128];
    int asks;
    char shown_title[128];
    char shown[1024];
    int shows;
    int choice; /* choose's reply */
    int choose_current, choose_n, chooses;
    AuListAction choose_action;
    bool reveal_ok;
    int reveals, rules_changed;
} Rec;

static void h_open(void *ctx, const char *url, bool new_tab)
{
    Rec *r = ctx;
    snprintf(r->url, sizeof r->url, "%s", url);
    r->new_tab = new_tab;
    r->opens++;
}

static char *h_ask(void *ctx, const char *prompt, const char *initial)
{
    Rec *r = ctx;
    snprintf(r->prompt, sizeof r->prompt, "%s", prompt);
    snprintf(r->initial, sizeof r->initial, "%s", initial);
    r->asks++;
    return r->answer ? au_strndup(r->answer, strlen(r->answer)) : NULL;
}

static void h_show(void *ctx, const char *title, const char *text)
{
    Rec *r = ctx;
    snprintf(r->shown_title, sizeof r->shown_title, "%s", title);
    snprintf(r->shown, sizeof r->shown, "%s", text);
    r->shows++;
}

static int h_choose(void *ctx, AuListAction action, const char *const *items, int n, int current)
{
    Rec *r = ctx;
    (void)items;
    r->choose_action = action;
    r->choose_n = n;
    r->choose_current = current;
    r->chooses++;
    return r->choice;
}

static bool h_reveal(void *ctx, const AuDownload *d)
{
    Rec *r = ctx;
    (void)d;
    r->reveals++;
    return r->reveal_ok;
}

static void h_rules(void *ctx)
{
    ((Rec *)ctx)->rules_changed++;
}

static AuListHost host_for(Rec *r)
{
    AuListHost h = {r, h_open, h_ask, h_show, h_choose, h_reveal, h_rules};
    return h;
}

/* ---- Window descriptions ---- */

static void test_descriptions(void)
{
    const char *const *heads;
    CHECK(au_list_columns(AU_LIST_HISTORY, &heads) == 4);
    CHECK_STR(heads[0], "Title");
    CHECK_STR(heads[2], "Address");
    CHECK(au_list_columns(AU_LIST_DOWNLOADS, &heads) == 5);
    CHECK_STR(heads[3], "Progress");
    CHECK(au_list_columns(AU_LIST_COOKIES, &heads) == 5);
    CHECK_STR(heads[1], "Domain");
    CHECK(au_list_columns(AU_LIST_BOOKMARKS, NULL) == 3);
    const AuListButton *b;
    CHECK(au_list_buttons(AU_LIST_BOOKMARKS, &b) == 4);
    CHECK(b[0].action == AU_LIST_OPEN && b[1].action == AU_LIST_NEW_FOLDER && b[2].action == AU_LIST_RENAME &&
          b[3].action == AU_LIST_DELETE);
    CHECK_STR(b[2].label, "Rename...");
    CHECK(au_list_buttons(AU_LIST_HISTORY, &b) == 3 && b[2].action == AU_LIST_CLEAR);
    CHECK_STR(b[2].label, "Clear History");
    CHECK(au_list_buttons(AU_LIST_DOWNLOADS, &b) == 5 && b[4].action == AU_LIST_REVEAL);
    CHECK_STR(b[2].label, "Remove");
    CHECK(au_list_buttons(AU_LIST_COOKIES, &b) == 4 && b[3].action == AU_LIST_SITE_RULE);
    CHECK_STR(au_list_title(AU_LIST_COOKIES), "Cookies");
}

/* ---- History ---- */

static void test_history(void)
{
    AuTime t0 = au_time_of_date(2026, 11, 14) + 10 * 3600; /* Saturday 10:00 */
    AuHistory h;
    au_history_init(&h, 100);
    au_history_visit(&h, "http://old.example/", "Old", t0 - 3 * 86400);
    au_history_visit(&h, "http://y.example/", "Yesterday", t0 - 86400 + 60);
    au_history_visit(&h, "http://b.example/", "", t0 - 3600);
    au_history_visit(&h, "http://a.example/", "A page", t0);
    au_history_visit(&h, "http://a.example/", NULL, t0); /* twice */
    AuListSources src = {0};
    src.history = &h;
    Rec rec = {0};
    AuListHost host = host_for(&rec);
    AuListModel m;
    au_list_init(&m, AU_LIST_HISTORY, &src, &host, t0);
    /* Today: a, b; Yesterday: y; Wednesday: old. */
    CHECK(m.n == 3 + 4);
    CHECK(m.items[0].type == AU_LIST_ITEM_GROUP && m.items[0].depth == 0);
    CHECK_STR(m.items[0].cols[0], "Today");
    CHECK(m.items[0].id == (AU_LIST_ID_GROUP | (uint32_t)au_day_of(t0)));
    CHECK(m.items[1].type == AU_LIST_ITEM_ENTRY && m.items[1].depth == 1 && m.items[1].icon == AU_LIST_ICON_PAGE);
    CHECK_STR(m.items[1].cols[0], "A page");
    CHECK_STR(m.items[1].cols[1], "10:00");
    CHECK_STR(m.items[1].cols[2], "http://a.example/");
    CHECK_STR(m.items[1].cols[3], "2");
    CHECK_STR(m.items[1].summary, "10:00  http://a.example/");
    CHECK_STR(m.items[2].cols[0], "http://b.example/"); /* no title: the URL */
    CHECK_STR(m.items[3].cols[0], "Yesterday");
    CHECK(m.items[3].type == AU_LIST_ITEM_GROUP);
    CHECK_STR(m.items[5].cols[0], "Wednesday");
    CHECK(m.items[1].id == h.e[0].id && m.items[1].id != 0 && m.items[2].id != m.items[1].id);

    /* The painted list built from it. */
    AuListView lv;
    au_listview_init(&lv);
    au_listview_from_list(&lv, &m);
    CHECK(lv.n == 7 && lv.rows[0].kind == AU_ROW_HEADER && lv.rows[1].kind == AU_ROW_LINK && lv.rows[1].indent == 1);
    CHECK_STR(lv.rows[1].detail, "10:00  http://a.example/");
    au_listview_free(&lv);

    /* Enable rules: nothing selected, a heading, an entry. */
    CHECK(!au_list_can(&m, AU_LIST_OPEN) && !au_list_can(&m, AU_LIST_DELETE) && au_list_can(&m, AU_LIST_CLEAR));
    CHECK(!au_list_can(&m, AU_LIST_RENAME) && !au_list_can(&m, AU_LIST_STOP)); /* not in this window */
    au_list_select(&m, 0);
    CHECK(!au_list_can(&m, AU_LIST_OPEN) && !au_list_can(&m, AU_LIST_DELETE));
    CHECK(au_list_activate(&m, 0, t0) == AU_LIST_NOTHING && rec.opens == 0);
    au_list_select(&m, 2);
    CHECK(au_list_can(&m, AU_LIST_OPEN) && au_list_can(&m, AU_LIST_OPEN_NEW_TAB) && au_list_can(&m, AU_LIST_DELETE));
    CHECK(au_list_run(&m, AU_LIST_OPEN, t0) == AU_LIST_DONE && rec.opens == 1 && !rec.new_tab);
    CHECK_STR(rec.url, "http://b.example/");
    CHECK(au_list_run(&m, AU_LIST_OPEN_NEW_TAB, t0) == AU_LIST_DONE && rec.new_tab);

    /* The change notice, and the selection follows its row. */
    uint32_t gen = m.generation, b_id = m.items[2].id;
    CHECK(!au_list_stale(&m));
    au_history_visit(&h, "http://new.example/", "New", t0 + 60);
    CHECK(au_list_stale(&m));
    au_list_refresh(&m, t0 + 60);
    CHECK(!au_list_stale(&m) && m.generation == gen + 1);
    CHECK(m.n == 8 && m.selected == 3 && m.items[3].id == b_id);

    /* Delete: the entry goes and the selection keeps its place. */
    CHECK(au_list_run(&m, AU_LIST_DELETE, t0 + 60) == AU_LIST_DONE);
    CHECK(h.n == 4 && au_history_find(&h, "http://b.example/") < 0);
    CHECK(m.n == 7 && m.selected == 3 && m.items[3].type == AU_LIST_ITEM_GROUP);
    CHECK(!au_list_stale(&m));

    /* Clear. */
    CHECK(au_list_run(&m, AU_LIST_CLEAR, t0) == AU_LIST_DONE);
    CHECK(h.n == 0 && m.n == 0 && m.selected == -1 && !au_list_can(&m, AU_LIST_CLEAR));
    CHECK(au_list_run(&m, AU_LIST_CLEAR, t0) == AU_LIST_NOTHING);
    au_list_free(&m);
    au_history_free(&h);
}

/* ---- Bookmarks ---- */

static void test_bookmarks(void)
{
    AuTime t0 = au_time_of_date(2026, 11, 14) + 10 * 3600;
    AuBookmarks bm;
    au_bookmarks_init(&bm);
    AuBookmark *a = au_bookmark_add(&bm, NULL, NULL, "Apple", "http://apple.example/", t0);
    AuBookmark *f = au_bookmark_add_folder(&bm, NULL, NULL, "Retro", t0);
    AuBookmark *g = au_bookmark_add(&bm, f, NULL, "Garden", "http://garden.example/", 0);
    AuBookmark *f2 = au_bookmark_add_folder(&bm, f, NULL, "Inner", 0);
    au_bookmark_add(&bm, f2, NULL, "Deep", "http://deep.example/", 0);
    au_bookmark_add_folder(&bm, NULL, NULL, "Empty", 0);
    AuListSources src = {0};
    src.bookmarks = &bm;
    Rec rec = {0};
    AuListHost host = host_for(&rec);
    AuListModel m;
    au_list_init(&m, AU_LIST_BOOKMARKS, &src, &host, t0);

    /* Closed folders hide their contents. */
    CHECK(m.n == 3);
    CHECK(m.items[0].type == AU_LIST_ITEM_ENTRY && m.items[0].id == a->id && m.items[0].depth == 0);
    CHECK_STR(m.items[0].cols[0], "Apple");
    CHECK_STR(m.items[0].cols[1], "http://apple.example/");
    CHECK_STR(m.items[0].cols[2], "2026-11-14 10:00");
    CHECK_STR(m.items[0].summary, "http://apple.example/");
    CHECK(m.items[1].type == AU_LIST_ITEM_FOLDER && !m.items[1].expanded && m.items[1].icon == AU_LIST_ICON_FOLDER);
    CHECK_STR(m.items[1].cols[1], "");

    /* Opening a folder (its triangle, or Open) shows its rows a level
     * deeper and keeps the selection. */
    au_list_select(&m, 1);
    au_list_toggle(&m, 1, t0);
    CHECK(m.n == 5 && m.items[1].expanded && m.selected == 1);
    CHECK(m.items[2].id == g->id && m.items[2].depth == 1);
    CHECK(m.items[3].id == f2->id && m.items[3].depth == 1 && m.items[3].type == AU_LIST_ITEM_FOLDER);
    CHECK(au_list_run(&m, AU_LIST_OPEN, t0) == AU_LIST_DONE && m.n == 3 && rec.opens == 0);
    au_list_toggle(&m, 1, t0);
    au_list_toggle(&m, 3, t0);
    CHECK(m.n == 6 && m.items[4].depth == 2);

    /* The painted list: a folder's triangle is hit at its indent. */
    AuListView lv;
    au_listview_init(&lv);
    au_listview_from_list(&lv, &m);
    bool disc;
    CHECK(lv.rows[3].kind == AU_ROW_FOLDER && lv.rows[3].indent == 1 && lv.rows[3].expanded);
    CHECK(au_listview_hit(&lv, 6 + 16 + 2, 2 + 3 * AU_LIST_ROW_H + 5, &disc) == 3 && disc);
    CHECK(lv.selected == 1);
    au_listview_free(&lv);

    /* Enable rules. */
    au_list_select(&m, -1);
    CHECK(!au_list_can(&m, AU_LIST_OPEN) && !au_list_can(&m, AU_LIST_RENAME) && !au_list_can(&m, AU_LIST_DELETE));
    CHECK(au_list_can(&m, AU_LIST_NEW_FOLDER) && !au_list_can(&m, AU_LIST_CLEAR));
    au_list_select(&m, 1);
    CHECK(au_list_can(&m, AU_LIST_OPEN) && !au_list_can(&m, AU_LIST_OPEN_NEW_TAB));
    au_list_select(&m, 0);
    CHECK(au_list_can(&m, AU_LIST_OPEN_NEW_TAB));
    CHECK(au_list_activate(&m, 0, t0) == AU_LIST_DONE && rec.opens == 1);
    CHECK_STR(rec.url, "http://apple.example/");

    /* New Folder asks for a name; cancelling does nothing. */
    rec.answer = NULL;
    uint32_t gen = bm.generation;
    CHECK(au_list_run(&m, AU_LIST_NEW_FOLDER, t0) == AU_LIST_NOTHING && rec.asks == 1 && bm.generation == gen);
    CHECK_STR(rec.prompt, "Name of the new folder:");
    CHECK_STR(rec.initial, "New Folder");
    /* With Garden selected the folder goes into Retro, and is selected. */
    au_list_select(&m, au_list_find(&m, g->id));
    rec.answer = "Stuff";
    CHECK(au_list_run(&m, AU_LIST_NEW_FOLDER, t0) == AU_LIST_DONE);
    CHECK(m.selected >= 0 && m.items[m.selected].type == AU_LIST_ITEM_FOLDER && m.items[m.selected].depth == 1);
    CHECK_STR(m.items[m.selected].cols[0], "Stuff");
    CHECK(f->last && !strcmp(f->last->title, "Stuff"));
    /* An empty name gives "New Folder". */
    au_list_select(&m, -1);
    rec.answer = "";
    CHECK(au_list_run(&m, AU_LIST_NEW_FOLDER, t0) == AU_LIST_DONE && !strcmp(bm.root.last->title, "New Folder"));

    /* Rename asks with the current name. */
    au_list_select(&m, au_list_find(&m, a->id));
    rec.answer = "Apple Computer";
    CHECK(au_list_run(&m, AU_LIST_RENAME, t0) == AU_LIST_DONE);
    CHECK_STR(rec.initial, "Apple");
    CHECK_STR(a->title, "Apple Computer");
    CHECK_STR(m.items[m.selected].cols[0], "Apple Computer");
    rec.answer = "";
    CHECK(au_list_run(&m, AU_LIST_RENAME, t0) == AU_LIST_NOTHING && !strcmp(a->title, "Apple Computer"));

    /* Move, as a drag would: Apple into Inner. */
    CHECK(!au_list_move_bookmark(&m, f->id, f2->id, 0, t0)); /* into its own child */
    CHECK(au_list_move_bookmark(&m, a->id, f2->id, 0, t0));
    CHECK(a->parent == f2 && m.items[au_list_find(&m, a->id)].depth == 2);

    /* Delete a folder: it goes with its contents, and nothing is
     * selected. */
    au_list_select(&m, au_list_find(&m, f->id));
    CHECK(au_list_run(&m, AU_LIST_DELETE, t0) == AU_LIST_DONE);
    CHECK(au_bookmarks_count(&bm) == 2 && m.n == 2 && m.selected == -1);
    CHECK(!au_list_stale(&m));
    au_bookmark_add(&bm, NULL, NULL, "Later", "http://later.example/", t0);
    CHECK(au_list_stale(&m));
    au_list_free(&m);
    au_bookmarks_free(&bm);
}

/* ---- Downloads ---- */

static void test_downloads(void)
{
    AuDownloads dl;
    au_downloads_init(&dl, au_sim_source());
    AuDownload *act = au_downloads_add(&dl, "http://x.example/a.sit", "a.sit", "", 2048, 100);
    AuDownload *done = au_downloads_add(&dl, "http://x.example/b.sit", "b.sit", "", 1024, 100);
    AuDownload *bad = au_downloads_add(&dl, "http://x.example/c.sit", "c.sit", "", -1, 100);
    AuDownload *wait = au_downloads_add(&dl, "http://x.example/d.sit", "d.sit", "", -1, 100);
    act->state = AU_DL_ACTIVE;
    act->received = 512;
    done->state = AU_DL_DONE;
    done->received = 1024;
    bad->state = AU_DL_FAILED;
    bad->received = 300;
    AuListSources src = {0};
    src.downloads = &dl;
    Rec rec = {0};
    AuListHost host = host_for(&rec);
    AuListModel m;
    au_list_init(&m, AU_LIST_DOWNLOADS, &src, &host, 100);
    CHECK(m.n == 4 && m.items[0].id == act->id && m.items[3].id == wait->id);
    CHECK(m.items[0].state == AU_DL_ACTIVE && m.items[0].progress == 250 && m.items[0].icon == AU_LIST_ICON_FILE);
    CHECK_STR(m.items[0].cols[0], "a.sit");
    CHECK_STR(m.items[0].cols[1], "http://x.example/a.sit");
    CHECK_STR(m.items[0].cols[3], "25%");
    CHECK_STR(m.items[0].cols[4], "2 KB");
    CHECK(m.items[1].state == AU_DL_DONE && m.items[1].progress == 1000);
    CHECK_STR(m.items[1].cols[2], "Done, 1 KB");
    CHECK(m.items[2].state == AU_DL_FAILED && m.items[2].progress == -1);
    CHECK_STR(m.items[2].cols[3], "");
    CHECK_STR(m.items[3].cols[2], "Waiting");
    CHECK_STR(m.items[3].summary, "Waiting");

    /* Enable rules by state. */
    CHECK(!au_list_can(&m, AU_LIST_STOP) && !au_list_can(&m, AU_LIST_DELETE) && au_list_can(&m, AU_LIST_CLEAR));
    au_list_select(&m, 0);
    CHECK(au_list_can(&m, AU_LIST_STOP) && !au_list_can(&m, AU_LIST_RESUME) && !au_list_can(&m, AU_LIST_REVEAL));
    au_list_select(&m, 1);
    CHECK(!au_list_can(&m, AU_LIST_STOP) && !au_list_can(&m, AU_LIST_RESUME) && au_list_can(&m, AU_LIST_REVEAL));
    au_list_select(&m, 2);
    CHECK(au_list_can(&m, AU_LIST_RESUME) && !au_list_can(&m, AU_LIST_REVEAL) && au_list_can(&m, AU_LIST_DELETE));
    au_list_select(&m, 3);
    CHECK(au_list_can(&m, AU_LIST_STOP) && !au_list_can(&m, AU_LIST_OPEN) && !au_list_can(&m, AU_LIST_RENAME));

    /* Stop, then resume. */
    CHECK(au_list_run(&m, AU_LIST_STOP, 100) == AU_LIST_DONE && wait->state == AU_DL_PAUSED);
    CHECK(m.items[3].state == AU_DL_PAUSED && au_list_can(&m, AU_LIST_RESUME));
    CHECK(au_list_run(&m, AU_LIST_RESUME, 100) == AU_LIST_DONE && wait->state == AU_DL_QUEUED);

    /* Reveal: the port's answer decides. */
    au_list_select(&m, 1);
    rec.reveal_ok = false;
    CHECK(au_list_run(&m, AU_LIST_REVEAL, 100) == AU_LIST_FAILED && rec.reveals == 1);
    rec.reveal_ok = true;
    CHECK(au_list_run(&m, AU_LIST_REVEAL, 100) == AU_LIST_DONE && rec.reveals == 2);

    /* Remove keeps the position; Clear Completed removes done and
     * failed. */
    CHECK(au_list_run(&m, AU_LIST_DELETE, 100) == AU_LIST_DONE && dl.n == 3);
    CHECK(m.n == 3 && m.selected == 1 && m.items[1].state == AU_DL_FAILED);
    CHECK(au_list_run(&m, AU_LIST_CLEAR, 100) == AU_LIST_DONE && dl.n == 2 && m.n == 2);
    CHECK(m.selected == 1 && !au_list_can(&m, AU_LIST_CLEAR));

    /* The change notice follows progress. */
    CHECK(!au_list_stale(&m));
    au_download_progress(&dl, act, 1024, 2048);
    CHECK(au_list_stale(&m));
    au_list_refresh(&m, 100);
    CHECK(m.items[0].progress == 500);
    au_list_free(&m);
    au_downloads_free(&dl);
}

/* ---- Cookies ---- */

#define NOW 1800000000LL

static void set_cookie(AuCookieJar *jar, const char *url, const char *sc)
{
    AuUrl u;
    if (!au_url_parse(&u, url, NULL)) {
        printf("bad test URL %s\n", url);
        exit(2);
    }
    AuCookieRequest rq;
    memset(&rq, 0, sizeof rq);
    rq.url = &u;
    rq.top_level_nav = true;
    rq.safe_method = true;
    rq.now = NOW;
    au_cookie_jar_set(jar, &rq, sc, NULL);
    au_url_free(&u);
}

static void test_cookies(void)
{
    AuCookieJar *jar = au_cookie_jar_new();
    set_cookie(jar, "https://www.b.example/", "one=1; Max-Age=3600; Secure; HttpOnly");
    set_cookie(jar, "https://img.b.example/", "two=2");
    set_cookie(jar, "https://a.example/", "three=3");
    au_cookie_jar_set_rule(jar, "c.example", AU_COOKIE_RULE_BLOCK);
    AuListSources src = {0};
    src.jar = jar;
    Rec rec = {0};
    AuListHost host = host_for(&rec);
    AuListModel m;
    au_list_init(&m, AU_LIST_COOKIES, &src, &host, NOW);

    /* A folder per site, sorted. */
    CHECK(m.n == 3);
    CHECK(m.items[0].type == AU_LIST_ITEM_FOLDER && !m.items[0].expanded && m.items[0].depth == 0);
    CHECK_STR(m.items[0].cols[0], "a.example");
    CHECK_STR(m.items[1].cols[0], "b.example");
    CHECK_STR(m.items[1].cols[4], "2 cookies");
    CHECK_STR(m.items[2].summary, "0 cookies, blocked");
    uint32_t b_id = m.items[1].id;
    CHECK(b_id != 0 && b_id != m.items[0].id);

    /* Enable rules. */
    CHECK(!au_list_can(&m, AU_LIST_DETAILS) && !au_list_can(&m, AU_LIST_DELETE) && au_list_can(&m, AU_LIST_CLEAR));
    CHECK(!au_list_can(&m, AU_LIST_SITE_RULE) && !au_list_can(&m, AU_LIST_OPEN));

    /* Activating a site opens it: its cookies at depth 1. */
    CHECK(au_list_activate(&m, 1, NOW) == AU_LIST_DONE);
    CHECK(m.n == 5 && m.items[1].expanded && m.selected == 1 && m.items[1].id == b_id);
    CHECK(m.items[2].type == AU_LIST_ITEM_ENTRY && m.items[2].depth == 1 && m.items[2].icon == AU_LIST_ICON_COOKIE);
    CHECK_STR(m.items[2].cols[0], "one");
    CHECK_STR(m.items[2].cols[1], "www.b.example");
    CHECK_STR(m.items[2].cols[2], "/");
    CHECK_STR(m.items[2].cols[3], "2027-01-15 09:00");
    CHECK_STR(m.items[3].cols[3], "session");
    CHECK(strstr(m.items[2].summary, "Secure  HttpOnly") != NULL);
    uint32_t one_id = m.items[2].id;

    /* Ids stay across a refresh. */
    au_list_touch(&m);
    CHECK(au_list_stale(&m));
    au_list_refresh(&m, NOW);
    CHECK(!au_list_stale(&m) && m.items[2].id == one_id && m.items[1].id == b_id);

    /* Activating a cookie shows its details through the port. */
    CHECK(au_list_activate(&m, 2, NOW) == AU_LIST_DONE && rec.shows == 1);
    CHECK_STR(rec.shown_title, "one");
    CHECK(strstr(rec.shown, "Name: one\nValue: 1\n") != NULL);
    CHECK(au_list_can(&m, AU_LIST_DETAILS) && au_list_can(&m, AU_LIST_SITE_RULE));
    CHECK(au_list_run(&m, AU_LIST_DETAILS, NOW) == AU_LIST_DONE && rec.shows == 2);

    /* Site Rule asks with the current rule checked; the same answer does
     * nothing, another sets it and tells the port. */
    rec.choice = AU_COOKIE_RULE_NONE;
    CHECK(au_list_run(&m, AU_LIST_SITE_RULE, NOW) == AU_LIST_NOTHING && rec.chooses == 1);
    CHECK(rec.choose_action == AU_LIST_SITE_RULE && rec.choose_n == 4 && rec.choose_current == AU_COOKIE_RULE_NONE);
    CHECK(rec.rules_changed == 0);
    rec.choice = AU_COOKIE_RULE_SESSION;
    CHECK(au_list_run(&m, AU_LIST_SITE_RULE, NOW) == AU_LIST_DONE);
    CHECK(au_cookie_jar_rule(jar, "b.example") == AU_COOKIE_RULE_SESSION && rec.rules_changed == 1);
    CHECK(strstr(m.items[1].summary, "session only") != NULL);
    rec.choice = -1;
    CHECK(au_list_run(&m, AU_LIST_SITE_RULE, NOW) == AU_LIST_NOTHING);

    /* Remove a cookie: its site becomes the selection. */
    au_list_select(&m, au_list_find(&m, one_id));
    CHECK(au_list_run(&m, AU_LIST_DELETE, NOW) == AU_LIST_DONE);
    CHECK(au_cookie_jar_count(jar) == 2 && m.n == 4 && m.selected == 1 && m.items[1].id == b_id);
    CHECK(au_list_find(&m, one_id) < 0);

    /* Change notice: a page sets a cookie. */
    CHECK(!au_list_stale(&m));
    set_cookie(jar, "https://d.example/", "four=4");
    CHECK(au_list_stale(&m));
    au_list_refresh(&m, NOW);
    CHECK(m.n == 5 && m.selected == 1 && m.items[1].id == b_id);

    /* Remove All keeps sites that have a rule. */
    CHECK(au_list_run(&m, AU_LIST_CLEAR, NOW) == AU_LIST_DONE);
    CHECK(au_cookie_jar_count(jar) == 0 && m.n == 2 && !au_list_can(&m, AU_LIST_CLEAR));

    /* Without a host, actions that ask do nothing. */
    AuListModel q;
    au_list_init(&q, AU_LIST_COOKIES, &src, NULL, NOW);
    au_list_select(&q, 0);
    CHECK(au_list_can(&q, AU_LIST_DETAILS) && au_list_run(&q, AU_LIST_DETAILS, NOW) == AU_LIST_NOTHING);
    CHECK(au_list_run(&q, AU_LIST_SITE_RULE, NOW) == AU_LIST_NOTHING);
    CHECK(au_list_set_site_rule(&q, AU_COOKIE_RULE_ALLOW, NOW) && au_cookie_jar_rule(jar, q.items[0].cols[0]) ==
                                                                       AU_COOKIE_RULE_ALLOW);
    au_list_free(&q);
    au_list_free(&m);

    /* No jar: an empty list with nothing enabled. */
    AuListSources none = {0};
    au_list_init(&m, AU_LIST_COOKIES, &none, &host, NOW);
    CHECK(m.n == 0 && !au_list_can(&m, AU_LIST_CLEAR) && !au_list_stale(&m));
    au_list_free(&m);
    au_cookie_jar_free(jar);
}

int main(void)
{
    test_descriptions();
    test_history();
    test_bookmarks();
    test_downloads();
    test_cookies();
    printf("lists: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
