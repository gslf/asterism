/*
 * test_routine.c — #routine parsing, strict validation with file:line
 * messages, canonical serialization round trip, save/load and load_dir.
 */

#include "astime_test.h"

static const char *EXAMPLE =
    "#routine {\n"
    "  name: \"morning-brief\",              // slug; must equal the file name\n"
    "  enabled: true,\n"
    "  trigger: { cron: \"0 8 * * 1-5\", tz: \"local\" },\n"
    "  watch: { url: \"https://example.com/page\", every: \"PT1H\" },\n"
    "  goal: \"Prepare the morning brief: calendar, news, repository status.\",\n"
    "  mode: \"automate\",                    // chat | coding | automate\n"
    "  profile: \"automation-ci\",\n"
    "  tools: [\"fs\", \"grep\"],\n"
    "  providers: [\"local\"],\n"
    "  notify: \"always\",\n"
    "  quiet_hours: \"22:00-07:00\",\n"
    "  deliver: { channel: \"telegram\", star: \"default\" },\n"
    "  max_runs_per_day: 24,\n"
    "}\n";

static const char *EXAMPLE_CANONICAL =
    "#routine {\n"
    "  name: \"morning-brief\",\n"
    "  enabled: true,\n"
    "  trigger: { cron: \"0 8 * * 1-5\", tz: \"local\" },\n"
    "  watch: { url: \"https://example.com/page\", every: \"PT1H\" },\n"
    "  goal: \"Prepare the morning brief: calendar, news, repository status.\",\n"
    "  mode: \"automate\",\n"
    "  profile: \"automation-ci\",\n"
    "  tools: [\"fs\", \"grep\"],\n"
    "  providers: [\"local\"],\n"
    "  notify: \"always\",\n"
    "  quiet_hours: \"22:00-07:00\",\n"
    "  deliver: { channel: \"telegram\", star: \"default\" },\n"
    "  max_runs_per_day: 24,\n"
    "}\n";

TEST(parses_the_documented_example) {
  astime_routine r;
  char err[ASTIME_ERR_SIZE];
  ASSERT_OK(astime_routine_parse(EXAMPLE, "morning-brief.xcdn", "morning-brief", &r, err));
  ASSERT_EQ_STR(r.name, "morning-brief");
  ASSERT_TRUE(r.enabled);
  ASSERT_EQ_INT(r.trigger.kind, ASTIME_TRIGGER_CRON);
  ASSERT_EQ_STR(r.trigger.cron_text, "0 8 * * 1-5");
  ASSERT_EQ_INT(r.trigger.tz, ASTIME_TZ_LOCAL);
  ASSERT_EQ_INT(r.watch_kind, ASTIME_WATCH_URL);
  ASSERT_EQ_STR(r.watch_target, "https://example.com/page");
  ASSERT_EQ_INT(r.watch_every_s, 3600);
  ASSERT_EQ_INT(r.mode, ASTIME_MODE_AUTOMATE);
  ASSERT_EQ_INT(r.profile, ASTIME_PROFILE_AUTOMATION_CI);
  ASSERT_TRUE(r.tools_set);
  ASSERT_EQ_INT(r.tools_n, 2);
  ASSERT_EQ_STR(r.tools[1], "grep");
  ASSERT_EQ_INT(r.providers, ASTIME_PROVIDER_LOCAL);
  ASSERT_EQ_INT(r.notify, ASTIME_NOTIFY_ALWAYS);
  ASSERT_TRUE(r.quiet_set);
  ASSERT_EQ_INT(r.quiet_start_min, 22 * 60);
  ASSERT_EQ_INT(r.quiet_end_min, 7 * 60);
  ASSERT_EQ_STR(r.deliver_channel, "telegram");
  ASSERT_EQ_STR(r.deliver_star, "default");
  ASSERT_EQ_INT(r.max_runs_per_day, 24);
  astime_routine_free(&r);
}

TEST(defaults_for_a_minimal_routine) {
  astime_routine r;
  char err[ASTIME_ERR_SIZE];
  ASSERT_OK(astime_routine_parse("name: \"x\", trigger: { every: r\"PT30M\" }, goal: \"g\"", NULL,
                                 NULL, &r, err));
  ASSERT_TRUE(!r.enabled);
  ASSERT_EQ_INT(r.trigger.kind, ASTIME_TRIGGER_EVERY);
  ASSERT_EQ_INT(r.trigger.every_s, 1800);
  ASSERT_EQ_INT(r.mode, ASTIME_MODE_CHAT);
  ASSERT_EQ_INT(r.profile, ASTIME_PROFILE_CHAT);
  ASSERT_TRUE(!r.tools_set);
  ASSERT_EQ_INT(r.providers, ASTIME_PROVIDER_LOCAL);
  ASSERT_EQ_INT(r.notify, ASTIME_NOTIFY_ALWAYS);
  ASSERT_TRUE(!r.quiet_set);
  ASSERT_TRUE(r.deliver_channel == NULL);
  ASSERT_EQ_INT(r.max_runs_per_day, 0);
  astime_routine_free(&r);
  /* the profile follows the mode, as in the engine */
  ASSERT_OK(astime_routine_parse("#routine { name: \"x\", trigger: { cron: \"@daily\" }, goal: \"g\", "
                                 "mode: \"coding\" }", NULL, NULL, &r, err));
  ASSERT_EQ_INT(r.profile, ASTIME_PROFILE_CODING_SANDBOXED);
  astime_routine_free(&r);
  ASSERT_OK(astime_routine_parse("#routine { name: \"x\", trigger: { at: t\"2026-11-02T09:00:00+01:00\" }, "
                                 "goal: \"g\", mode: \"automate\" }", NULL, NULL, &r, err));
  ASSERT_EQ_INT(r.profile, ASTIME_PROFILE_AUTOMATION_CI);
  ASSERT_EQ_INT(r.trigger.at, utc(2026, 11, 2, 8, 0));
  ASSERT_EQ_INT(r.trigger.at_offset_s, 3600);
  astime_routine_free(&r);
  /* watch-only routine */
  ASSERT_OK(astime_routine_parse("#routine { name: \"w\", watch: { file: \"/tmp/x\", every: \"PT10M\" }, "
                                 "goal: \"g\" }", NULL, NULL, &r, err));
  ASSERT_EQ_INT(r.trigger.kind, ASTIME_TRIGGER_NONE);
  ASSERT_EQ_INT(r.watch_kind, ASTIME_WATCH_FILE);
  ASSERT_EQ_INT(r.watch_every_s, 600);
  astime_routine_free(&r);
}

/* Each case: text, expected code, expected substring of the message. */
TEST(validation_errors_are_precise) {
  static const struct {
    const char *text;
    astime_err code;
    const char *msg;
  } cases[] = {
      {"#routine {\n  name: \"r\",\n  goal: \"g\",\n  trigger: { cron: \"@daily\" },\n  colour: 1,\n}",
       ASTIME_ERR_INVALID, "r.xcdn:5: colour: unknown key \"colour\" (routine keys: name, enabled"},
      {"#routine {\n  name: \"r\",\n  enabled: \"yes\",\n}", ASTIME_ERR_INVALID,
       "r.xcdn:3: enabled: expected true or false, got a string"},
      {"#routine {\n  name: \"r\",\n  goal: \"g\",\n  trigger: {\n    cron: \"61 8 * * *\",\n  },\n}",
       ASTIME_ERR_INVALID, "r.xcdn:5: trigger.cron: minute: value 61 out of range 0-59"},
      {"#routine {\n  name: \"r\",\n  goal: \"g\",\n  trigger: { cron: \"0 0 30 2 *\" },\n}",
       ASTIME_ERR_INVALID, "r.xcdn:4: trigger.cron: \"0 0 30 2 *\" never fires"},
      {"#routine {\n  name: \"other\",\n  goal: \"g\",\n  trigger: { cron: \"@daily\" },\n}",
       ASTIME_ERR_INVALID, "r.xcdn:2: name: \"other\" must equal the file name \"r\""},
      {"#routine {\n  name: \"r\",\n  trigger: { cron: \"@daily\" },\n}", ASTIME_ERR_INVALID,
       "r.xcdn: goal: missing (required)"},
      {"#routine {\n  name: \"r\",\n  goal: \"g\",\n}", ASTIME_ERR_INVALID,
       "trigger: missing: a routine needs a trigger, a watch, or both"},
      {"#routine { name: \"r\", goal: \"g\", trigger: { cron: \"@daily\", every: \"PT1H\" } }",
       ASTIME_ERR_INVALID, "trigger: \"cron\", \"every\" and \"at\" are exclusive"},
      {"#routine { name: \"r\", goal: \"g\", trigger: { tz: \"utc\" } }", ASTIME_ERR_INVALID,
       "trigger: needs one of \"cron\", \"every\" or \"at\""},
      {"#routine { name: \"r\", goal: \"g\", trigger: { cron: \"@daily\", tz: \"Europe/Rome\" } }",
       ASTIME_ERR_INVALID, "trigger.tz: unknown value \"Europe/Rome\" (expected \"local\" or \"utc\")"},
      {"#routine { name: \"r\", goal: \"g\", trigger: { every: \"PT30S\" } }", ASTIME_ERR_INVALID,
       "trigger.every: must be at least PT1M"},
      {"#routine { name: \"r\", goal: \"g\", trigger: { every: \"P1M\" } }", ASTIME_ERR_INVALID,
       "trigger.every: invalid ISO 8601 duration \"P1M\""},
      {"#routine { name: \"r\", goal: \"g\", trigger: { at: \"tomorrow\" } }", ASTIME_ERR_INVALID,
       "trigger.at: invalid RFC 3339 time \"tomorrow\""},
      {"#routine { name: \"r\", goal: \"g\", trigger: { cron: \"@daily\", when: 1 } }",
       ASTIME_ERR_INVALID, "trigger.when: unknown key \"when\" (trigger keys: cron, every, at, tz)"},
      {"#routine { name: \"r\", goal: \"g\", watch: { url: \"ftp://x\", every: \"PT1H\" } }",
       ASTIME_ERR_INVALID, "watch.url: \"ftp://x\" is not an http(s) URL"},
      {"#routine { name: \"r\", goal: \"g\", watch: { file: \"rel/path\", every: \"PT1H\" } }",
       ASTIME_ERR_INVALID, "watch.file: \"rel/path\" is not an absolute path"},
      {"#routine { name: \"r\", goal: \"g\", watch: { url: \"https://x.it\" } }", ASTIME_ERR_INVALID,
       "watch.every: missing (required with a watch)"},
      {"#routine { name: \"r\", goal: \"g\", watch: { url: \"https://x.it\", file: \"/a\", every: \"PT1H\" } }",
       ASTIME_ERR_INVALID, "watch.file: \"url\" and \"file\" are exclusive"},
      {"#routine { name: \"r\", goal: \"g\", trigger: { cron: \"@daily\" }, mode: \"auto\" }",
       ASTIME_ERR_INVALID, "mode: unknown value \"auto\" (expected \"chat\", \"coding\" or \"automate\")"},
      {"#routine { name: \"r\", goal: \"g\", trigger: { cron: \"@daily\" }, profile: \"root\" }",
       ASTIME_ERR_INVALID, "profile: unknown value \"root\""},
      {"#routine { name: \"r\", goal: \"g\", trigger: { cron: \"@daily\" }, tools: [\"fs\", \"fs\"] }",
       ASTIME_ERR_INVALID, "tools: \"fs\" listed twice"},
      {"#routine { name: \"r\", goal: \"g\", trigger: { cron: \"@daily\" }, tools: [\"Bad Tool\"] }",
       ASTIME_ERR_INVALID, "tools: \"Bad Tool\" is not a tool package id"},
      {"#routine { name: \"r\", goal: \"g\", trigger: { cron: \"@daily\" }, tools: \"fs\" }",
       ASTIME_ERR_INVALID, "tools: expected a list of strings, got a string"},
      {"#routine { name: \"r\", goal: \"g\", trigger: { cron: \"@daily\" }, providers: [\"openai\"] }",
       ASTIME_ERR_INVALID, "providers: unknown provider \"openai\""},
      {"#routine { name: \"r\", goal: \"g\", trigger: { cron: \"@daily\" }, providers: [] }",
       ASTIME_ERR_INVALID, "providers: must allow at least one provider"},
      {"#routine { name: \"r\", goal: \"g\", trigger: { cron: \"@daily\" }, notify: \"sometimes\" }",
       ASTIME_ERR_INVALID, "notify: unknown value \"sometimes\""},
      {"#routine { name: \"r\", goal: \"g\", trigger: { cron: \"@daily\" }, quiet_hours: \"22-7\" }",
       ASTIME_ERR_INVALID, "quiet_hours: expected \"HH:MM-HH:MM\""},
      {"#routine { name: \"r\", goal: \"g\", trigger: { cron: \"@daily\" }, quiet_hours: \"25:00-07:00\" }",
       ASTIME_ERR_INVALID, "quiet_hours: expected \"HH:MM-HH:MM\""},
      {"#routine { name: \"r\", goal: \"g\", trigger: { cron: \"@daily\" }, deliver: { star: \"a\" } }",
       ASTIME_ERR_INVALID, "deliver.channel: missing (required)"},
      {"#routine { name: \"r\", goal: \"g\", trigger: { cron: \"@daily\" }, deliver: { channel: \"tg\", to: 1 } }",
       ASTIME_ERR_INVALID, "deliver.to: unknown key \"to\" (deliver keys: channel, star)"},
      {"#routine { name: \"r\", goal: \"g\", trigger: { cron: \"@daily\" }, max_runs_per_day: 0 }",
       ASTIME_ERR_INVALID, "max_runs_per_day: 0 out of range 1-1440"},
      {"#routine { name: \"r\", goal: \"g\", trigger: { cron: \"@daily\" }, max_runs_per_day: 2.5 }",
       ASTIME_ERR_INVALID, "max_runs_per_day: expected an integer, got a float"},
      {"#routine { name: \"R!\", goal: \"g\", trigger: { cron: \"@daily\" } }", ASTIME_ERR_INVALID,
       "name: \"R!\" is not a slug"},
      {"#routine { name: \"r\", goal: \"   \", trigger: { cron: \"@daily\" } }", ASTIME_ERR_INVALID,
       "goal: missing (required)"},
      {"#job { name: \"r\", goal: \"g\", trigger: { cron: \"@daily\" } }", ASTIME_ERR_INVALID,
       "r.xcdn: unexpected tag #job (expected #routine)"},
      {"#routine {\n  name: \"r\",\n  goal: \"g\"\n  trigger { cron: \"@daily\" },\n}",
       ASTIME_ERR_PARSE, "r.xcdn:4:"},
      {"[1, 2]", ASTIME_ERR_INVALID, "no #routine object found"},
  };
  size_t i;
  for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    astime_routine r;
    char err[ASTIME_ERR_SIZE];
    err[0] = '\0';
    ASSERT_ERR(astime_routine_parse(cases[i].text, "r.xcdn", "r", &r, err), cases[i].code);
    ASSERT_CONTAINS(err, cases[i].msg);
  }
}

TEST(serialize_is_canonical_and_round_trips) {
  astime_routine r, r2;
  char *text = NULL, *text2 = NULL, err[ASTIME_ERR_SIZE];
  ASSERT_OK(astime_routine_parse(EXAMPLE, NULL, NULL, &r, err));
  ASSERT_OK(astime_routine_serialize(&r, &text));
  ASSERT_EQ_STR(text, EXAMPLE_CANONICAL);
  ASSERT_OK(astime_routine_parse(text, NULL, "morning-brief", &r2, err));
  ASSERT_OK(astime_routine_serialize(&r2, &text2));
  ASSERT_EQ_STR(text2, text);
  astime_free(text);
  astime_free(text2);
  astime_routine_free(&r);
  astime_routine_free(&r2);
}

TEST(serialize_escapes_and_typed_values) {
  astime_routine r, back;
  char *text = NULL, err[ASTIME_ERR_SIZE];
  astime_routine_init(&r);
  r.name = core_strdup("odd-goal");
  r.goal = core_strdup("line one\nsays \"hi\" \\ tab\there — è");
  r.trigger.kind = ASTIME_TRIGGER_AT;
  r.trigger.tz = ASTIME_TZ_UTC;
  r.trigger.at = utc(2026, 11, 2, 8, 0);
  r.trigger.at_offset_s = 3600;
  r.watch_kind = ASTIME_WATCH_FILE;
  r.watch_target = core_strdup("/var/log/x.log");
  r.watch_every_s = 5400;
  r.providers = ASTIME_PROVIDER_LOCAL | ASTIME_PROVIDER_CODEX | ASTIME_PROVIDER_CLAUDE_CODE;
  r.tools_set = true; /* explicit empty allowlist */
  r.notify = ASTIME_NOTIFY_ON_CHANGE;
  r.quiet_set = true;
  r.quiet_start_min = 0;
  r.quiet_end_min = 1440;
  r.deliver_channel = core_strdup("telegram");
  ASSERT_OK(astime_routine_serialize(&r, &text));
  ASSERT_CONTAINS(text, "goal: \"line one\\nsays \\\"hi\\\" \\\\ tab\\there — è\",\n");
  ASSERT_CONTAINS(text, "trigger: { at: \"2026-11-02T09:00:00+01:00\", tz: \"utc\" },\n");
  ASSERT_CONTAINS(text, "watch: { file: \"/var/log/x.log\", every: \"PT1H30M\" },\n");
  ASSERT_CONTAINS(text, "tools: [],\n");
  ASSERT_CONTAINS(text, "providers: [\"local\", \"codex\", \"claude-code\"],\n");
  ASSERT_CONTAINS(text, "notify: \"on_change\",\n");
  ASSERT_CONTAINS(text, "quiet_hours: \"00:00-24:00\",\n");
  ASSERT_CONTAINS(text, "deliver: { channel: \"telegram\" },\n");
  ASSERT_TRUE(strstr(text, "max_runs_per_day") == NULL);
  ASSERT_OK(astime_routine_parse(text, NULL, "odd-goal", &back, err));
  ASSERT_EQ_STR(back.goal, r.goal);
  ASSERT_EQ_INT(back.trigger.at, r.trigger.at);
  ASSERT_EQ_INT(back.trigger.at_offset_s, 3600);
  ASSERT_EQ_INT(back.watch_every_s, 5400);
  ASSERT_TRUE(back.tools_set);
  ASSERT_EQ_INT(back.tools_n, 0);
  ASSERT_EQ_INT(back.providers, r.providers);
  ASSERT_EQ_INT(back.quiet_end_min, 1440);
  astime_free(text);
  astime_routine_free(&back);
  /* invalid routines are not serialized */
  free(r.goal);
  r.goal = NULL;
  ASSERT_ERR(astime_routine_serialize(&r, &text), ASTIME_ERR_INVALID);
  ASSERT_ERR(astime_routine_validate(&r, err), ASTIME_ERR_INVALID);
  ASSERT_EQ_STR(err, "goal: missing (required)");
  astime_routine_free(&r);
}

TEST(save_load_and_copy) {
  char dir[256], path[512], err[ASTIME_ERR_SIZE];
  astime_routine r, loaded, copy;
  char *disk;
  ASSERT_TRUE(test_tmpdir(dir));
  ASSERT_OK(astime_routine_parse(EXAMPLE, NULL, NULL, &r, err));
  ASSERT_OK(astime_routine_save(dir, &r, err));
  snprintf(path, sizeof path, "%s/morning-brief.xcdn", dir);
  disk = test_read_file(path, NULL);
  ASSERT_EQ_STR(disk, EXAMPLE_CANONICAL);
  free(disk);
  ASSERT_OK(astime_routine_load(path, &loaded, err));
  ASSERT_EQ_STR(loaded.goal, r.goal);
  ASSERT_OK(astime_routine_copy(&loaded, &copy));
  ASSERT_EQ_STR(copy.tools[0], "fs");
  ASSERT_TRUE(copy.tools[0] != loaded.tools[0]);
  astime_routine_free(&copy);
  astime_routine_free(&loaded);
  /* a file whose name differs from the routine name is rejected */
  snprintf(path, sizeof path, "%s/other.xcdn", dir);
  ASSERT_TRUE(test_write(dir, "other.xcdn", EXAMPLE));
  ASSERT_ERR(astime_routine_load(path, &loaded, err), ASTIME_ERR_INVALID);
  ASSERT_CONTAINS(err, "other.xcdn:2: name: \"morning-brief\" must equal the file name \"other\"");
  snprintf(path, sizeof path, "%s/missing.xcdn", dir);
  ASSERT_ERR(astime_routine_load(path, &loaded, err), ASTIME_ERR_NOT_FOUND);
  astime_routine_free(&r);
  test_rmtree(dir);
}

TEST(load_dir_skips_and_reports) {
  char dir[256];
  astime_routine *list = NULL;
  size_t n = 0;
  astime_strings issues = {NULL, 0};
  ASSERT_TRUE(test_tmpdir(dir));
  ASSERT_TRUE(test_write(dir, "b.xcdn", "#routine { name: \"b\", trigger: { cron: \"@daily\" }, goal: \"B\" }"));
  ASSERT_TRUE(test_write(dir, "a.xcdn", "#routine { name: \"a\", trigger: { every: \"PT1H\" }, goal: \"A\" }"));
  ASSERT_TRUE(test_write(dir, "broken.xcdn", "#routine {\n  name: \"broken\",\n  goal: 3,\n}"));
  ASSERT_TRUE(test_write(dir, "Bad Name.xcdn", "#routine { name: \"x\" }"));
  ASSERT_TRUE(test_write(dir, "notes.txt", "not a routine"));
  ASSERT_TRUE(test_write(dir, ".hidden.xcdn", "garbage"));
  ASSERT_OK(astime_load_dir(dir, &list, &n, &issues));
  ASSERT_EQ_INT(n, 2);
  ASSERT_EQ_STR(list[0].name, "a");
  ASSERT_EQ_STR(list[1].name, "b");
  ASSERT_EQ_INT(issues.n, 2);
  ASSERT_CONTAINS(issues.items[0], "Bad Name.xcdn: file name \"Bad Name\" is not a slug");
  ASSERT_CONTAINS(issues.items[1], "broken.xcdn:3: goal: expected a string, got an integer");
  astime_routines_free(list, n);
  astime_strings_free(&issues);
  /* a missing directory is empty */
  {
    char missing[300];
    snprintf(missing, sizeof missing, "%s/nope", dir);
    ASSERT_OK(astime_load_dir(missing, &list, &n, NULL));
    ASSERT_EQ_INT(n, 0);
  }
  test_rmtree(dir);
}

TEST_LIST = {
    TEST_ENTRY(parses_the_documented_example),
    TEST_ENTRY(defaults_for_a_minimal_routine),
    TEST_ENTRY(validation_errors_are_precise),
    TEST_ENTRY(serialize_is_canonical_and_round_trips),
    TEST_ENTRY(serialize_escapes_and_typed_values),
    TEST_ENTRY(save_load_and_copy),
    TEST_ENTRY(load_dir_skips_and_reports),
};

RUN_ALL_TESTS()
