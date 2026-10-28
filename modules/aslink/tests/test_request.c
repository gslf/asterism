/*
 * test_request.c — exact request bodies and argument validation, plus the
 * context parameters. No network: the builders are pure.
 */

#include "aslink_test.h"

/* Each test declares char *body_; the macro reuses it. */
#define BODY_IS(call, want)                                                 \
  do {                                                                      \
    body_ = NULL;                                                           \
    ASSERT_OK(call);                                                        \
    ASSERT_EQ_STR(body_, want);                                             \
    free(body_);                                                            \
    body_ = NULL;                                                           \
  } while (0)

TEST(get_updates_body) {
  char *body_ = NULL;
  BODY_IS(aslink_req_get_updates(0, 25, &body_, NULL),
          "{\"timeout\":25,\"allowed_updates\":[\"message\",\"callback_query\","
          "\"stopped_message_generation\"]}");
  BODY_IS(aslink_req_get_updates(727361003, 0, &body_, NULL),
          "{\"offset\":727361003,\"timeout\":0,\"allowed_updates\":[\"message\","
          "\"callback_query\",\"stopped_message_generation\"]}");
  ASSERT_ERR(aslink_req_get_updates(0, -1, &body_, NULL), ASLINK_ERR_INVALID);
  ASSERT_ERR(aslink_req_get_updates(0, 601, &body_, NULL), ASLINK_ERR_INVALID);
  ASSERT_TRUE(body_ == NULL);
}

TEST(send_message_body) {
  static const aslink_button buttons[] = {
    {"Yes \xe2\x9c\x93", "ap:1:ok", NULL},
    {"No", "ap:1:no", NULL},
    {"Evidenza", NULL, "https://example.org/e?a=1&b=2"},
  };
  aslink_keyboard kb = {buttons, 3, 2};
  aslink_send_opts o;
  char *body_ = NULL;
  BODY_IS(aslink_req_send_message(5, 0, "hello", NULL, &body_, NULL),
          "{\"chat_id\":5,\"text\":\"hello\"}");
  memset(&o, 0, sizeof o);
  o.parse_mode = "HTML";
  o.keyboard = &kb;
  o.reply_to = 42;
  o.silent = 1;
  o.no_preview = 1;
  BODY_IS(aslink_req_send_message(-1001234567890LL, 7, "<b>s\xc3\xac</b>\n\"q\"", &o,
                                  &body_, NULL),
          "{\"chat_id\":-1001234567890,\"message_thread_id\":7,"
          "\"text\":\"<b>s\xc3\xac</b>\\n\\\"q\\\"\",\"parse_mode\":\"HTML\","
          "\"link_preview_options\":{\"is_disabled\":true},"
          "\"disable_notification\":true,"
          "\"reply_parameters\":{\"message_id\":42,\"allow_sending_without_reply\":true},"
          "\"reply_markup\":{\"inline_keyboard\":["
          "[{\"text\":\"Yes \xe2\x9c\x93\",\"callback_data\":\"ap:1:ok\"},"
          "{\"text\":\"No\",\"callback_data\":\"ap:1:no\"}],"
          "[{\"text\":\"Evidenza\",\"url\":\"https://example.org/e?a=1&b=2\"}]]}}");
  /* per_row 0: one row */
  kb.per_row = 0;
  kb.n = 2;
  memset(&o, 0, sizeof o);
  o.keyboard = &kb;
  BODY_IS(aslink_req_send_message(5, 0, "x", &o, &body_, NULL),
          "{\"chat_id\":5,\"text\":\"x\",\"reply_markup\":{\"inline_keyboard\":["
          "[{\"text\":\"Yes \xe2\x9c\x93\",\"callback_data\":\"ap:1:ok\"},"
          "{\"text\":\"No\",\"callback_data\":\"ap:1:no\"}]]}}");
}

TEST(send_message_validation) {
  char *body = NULL, *big;
  aslink_status st;
  aslink_send_opts o;
  aslink_button bad_both = {"x", "data", "https://e"};
  aslink_button bad_none = {"x", NULL, NULL};
  aslink_button bad_data = {"x", "0123456789012345678901234567890123456789012345678901234567890123456789", NULL};
  aslink_button bad_url = {"x", NULL, "javascript:alert(1)"};
  aslink_keyboard kb = {&bad_both, 1, 0};
  memset(&o, 0, sizeof o);
  ASSERT_ERR(aslink_req_send_message(0, 0, "x", NULL, &body, &st), ASLINK_ERR_INVALID);
  ASSERT_ERR(aslink_req_send_message(5, -1, "x", NULL, &body, &st), ASLINK_ERR_INVALID);
  ASSERT_ERR(aslink_req_send_message(5, 0, NULL, NULL, &body, &st), ASLINK_ERR_INVALID);
  ASSERT_ERR(aslink_req_send_message(5, 0, "", NULL, &body, &st), ASLINK_ERR_INVALID);
  ASSERT_ERR(aslink_req_send_message(5, 0, "bad \xff", NULL, &body, &st),
             ASLINK_ERR_INVALID);
  ASSERT_TRUE(strstr(st.message, "UTF-8") != NULL);
  big = (char *)malloc(4098);
  ASSERT_TRUE(big != NULL);
  memset(big, 'a', 4097);
  big[4097] = 0;
  ASSERT_ERR(aslink_req_send_message(5, 0, big, NULL, &body, &st), ASLINK_ERR_TOO_LARGE);
  ASSERT_EQ_STR(st.message, "text has 4097 characters, the limit is 4096");
  /* with a parse_mode the server counts after parsing: allowed locally */
  o.parse_mode = "HTML";
  ASSERT_OK(aslink_req_send_message(5, 0, big, &o, &body, &st));
  free(body);
  body = NULL;
  free(big);
  o.parse_mode = "Markdown";
  ASSERT_ERR(aslink_req_send_message(5, 0, "x", &o, &body, &st), ASLINK_ERR_INVALID);
  o.parse_mode = NULL;
  o.keyboard = &kb;
  ASSERT_ERR(aslink_req_send_message(5, 0, "x", &o, &body, &st), ASLINK_ERR_INVALID);
  kb.buttons = &bad_none;
  ASSERT_ERR(aslink_req_send_message(5, 0, "x", &o, &body, &st), ASLINK_ERR_INVALID);
  kb.buttons = &bad_data;
  ASSERT_ERR(aslink_req_send_message(5, 0, "x", &o, &body, &st), ASLINK_ERR_INVALID);
  kb.buttons = &bad_url;
  ASSERT_ERR(aslink_req_send_message(5, 0, "x", &o, &body, &st), ASLINK_ERR_INVALID);
  kb.buttons = NULL;
  ASSERT_ERR(aslink_req_send_message(5, 0, "x", &o, &body, &st), ASLINK_ERR_INVALID);
  kb.buttons = &bad_none;
  kb.n = 0;
  ASSERT_ERR(aslink_req_send_message(5, 0, "x", &o, &body, &st), ASLINK_ERR_INVALID);
  ASSERT_TRUE(body == NULL);
}

TEST(edit_bodies) {
  static const aslink_button b[] = {{"Ok", "done", NULL}};
  aslink_keyboard kb = {b, 1, 1};
  aslink_send_opts o;
  char *body_ = NULL;
  memset(&o, 0, sizeof o);
  o.keyboard = &kb;
  o.no_preview = 1;
  o.silent = 1;   /* not an edit option: ignored */
  o.reply_to = 3; /* idem */
  BODY_IS(aslink_req_edit_message(5, 77, "Status: 3/5", &o, &body_, NULL),
          "{\"chat_id\":5,\"message_id\":77,\"text\":\"Status: 3/5\","
          "\"link_preview_options\":{\"is_disabled\":true},"
          "\"reply_markup\":{\"inline_keyboard\":[[{\"text\":\"Ok\",\"callback_data\":\"done\"}]]}}");
  BODY_IS(aslink_req_edit_keyboard(5, 77, &kb, &body_, NULL),
          "{\"chat_id\":5,\"message_id\":77,\"reply_markup\":{\"inline_keyboard\":"
          "[[{\"text\":\"Ok\",\"callback_data\":\"done\"}]]}}");
  BODY_IS(aslink_req_edit_keyboard(5, 77, NULL, &body_, NULL),
          "{\"chat_id\":5,\"message_id\":77}");
  ASSERT_ERR(aslink_req_edit_message(5, 0, "x", NULL, &body_, NULL), ASLINK_ERR_INVALID);
  ASSERT_ERR(aslink_req_edit_keyboard(5, -2, NULL, &body_, NULL), ASLINK_ERR_INVALID);
}

TEST(draft_body) {
  aslink_draft_opts o = {"HTML", 1, 1};
  char *body_ = NULL;
  BODY_IS(aslink_req_send_draft(5, 0, 8812, "Sto pensando", NULL, &body_, NULL),
          "{\"chat_id\":5,\"draft_id\":8812,\"text\":\"Sto pensando\"}");
  BODY_IS(aslink_req_send_draft(5, 100, -3, NULL, &o, &body_, NULL),
          "{\"chat_id\":5,\"message_thread_id\":100,\"draft_id\":-3,\"text\":\"\","
          "\"parse_mode\":\"HTML\",\"can_stop\":true,\"keep_on_stop\":true}");
  ASSERT_ERR(aslink_req_send_draft(5, 0, 0, "x", NULL, &body_, NULL), ASLINK_ERR_INVALID);
  ASSERT_ERR(aslink_req_send_draft(-100, 0, 1, "x", NULL, &body_, NULL),
             ASLINK_ERR_INVALID); /* private chats only */
}

TEST(small_method_bodies) {
  static const aslink_command cmds[] = {
    {"status", "Task status"},
    {"stop", "Stop the current task"},
  };
  aslink_command bad = {"Status", "x"};
  aslink_command bad2 = {"status", ""};
  char *body_ = NULL;
  BODY_IS(aslink_req_chat_action(5, 9, "typing", &body_, NULL),
          "{\"chat_id\":5,\"message_thread_id\":9,\"action\":\"typing\"}");
  ASSERT_ERR(aslink_req_chat_action(5, 0, "dancing", &body_, NULL), ASLINK_ERR_INVALID);
  BODY_IS(aslink_req_answer_callback("4382bfdwdsb323b2d9", "Approvato", 1, &body_, NULL),
          "{\"callback_query_id\":\"4382bfdwdsb323b2d9\",\"text\":\"Approvato\","
          "\"show_alert\":true}");
  BODY_IS(aslink_req_answer_callback("q", NULL, 0, &body_, NULL),
          "{\"callback_query_id\":\"q\"}");
  ASSERT_ERR(aslink_req_answer_callback("bad id", NULL, 0, &body_, NULL),
             ASLINK_ERR_INVALID);
  BODY_IS(aslink_req_create_topic(5, "Task: report Q3", &body_, NULL),
          "{\"chat_id\":5,\"name\":\"Task: report Q3\"}");
  ASSERT_ERR(aslink_req_create_topic(5, "", &body_, NULL), ASLINK_ERR_INVALID);
  BODY_IS(aslink_req_set_commands(cmds, 2, &body_, NULL),
          "{\"commands\":[{\"command\":\"status\",\"description\":\"Task status\"},"
          "{\"command\":\"stop\",\"description\":\"Stop the current task\"}]}");
  ASSERT_ERR(aslink_req_set_commands(&bad, 1, &body_, NULL), ASLINK_ERR_INVALID);
  ASSERT_ERR(aslink_req_set_commands(&bad2, 1, &body_, NULL), ASLINK_ERR_INVALID);
  ASSERT_ERR(aslink_req_set_commands(cmds, 0, &body_, NULL), ASLINK_ERR_INVALID);
  BODY_IS(aslink_req_get_file("BQACAgQAAxkBAAIBdoc", &body_, NULL),
          "{\"file_id\":\"BQACAgQAAxkBAAIBdoc\"}");
  ASSERT_ERR(aslink_req_get_file("../x y", &body_, NULL), ASLINK_ERR_INVALID);
}

TEST(length_limits) {
  char name[200], answer[260];
  aslink_status st;
  char *body = NULL;
  memset(name, 'n', 129);
  name[129] = 0;
  ASSERT_ERR(aslink_req_create_topic(5, name, &body, &st), ASLINK_ERR_TOO_LARGE);
  name[128] = 0;
  ASSERT_OK(aslink_req_create_topic(5, name, &body, &st));
  free(body);
  memset(answer, 'a', 201);
  answer[201] = 0;
  ASSERT_ERR(aslink_req_answer_callback("q", answer, 0, &body, &st), ASLINK_ERR_TOO_LARGE);
  ASSERT_OK(aslink_check_text("caption", NULL, ASLINK_CAPTION_MAX, 1, &st));
  ASSERT_OK(aslink_check_text("caption", "", ASLINK_CAPTION_MAX, 1, &st));
}

TEST(open_params) {
  aslink_telegram_params p;
  aslink_ctx *c = (aslink_ctx *)1;
  memset(&p, 0, sizeof p);
  p.token = "123456:AAEhBP0av28jTqW-lLTK_x";
  ASSERT_OK(aslink_telegram_open(&p, &c));
  ASSERT_EQ_STR(c->base, "https://api.telegram.org");
  ASSERT_EQ_INT(c->request_timeout_ms, 30000);
  aslink_close(c);
  p.api_base = "http://127.0.0.1:8081///";
  ASSERT_OK(aslink_telegram_open(&p, &c));
  ASSERT_EQ_STR(c->base, "http://127.0.0.1:8081");
  aslink_close(c);
  p.api_base = "http://LocalHost:9/api";
  ASSERT_OK(aslink_telegram_open(&p, &c));
  aslink_close(c);
  p.api_base = "http://[::1]:9";
  ASSERT_OK(aslink_telegram_open(&p, &c));
  aslink_close(c);
  p.api_base = "https://tg.example.org/proxy";
  ASSERT_OK(aslink_telegram_open(&p, &c));
  aslink_close(c);
  /* plain http beyond loopback would expose the token */
  p.api_base = "http://192.168.1.10:8081";
  ASSERT_ERR(aslink_telegram_open(&p, &c), ASLINK_ERR_INVALID);
  ASSERT_TRUE(c == NULL);
  p.api_base = "http://localhost.evil.com";
  ASSERT_ERR(aslink_telegram_open(&p, &c), ASLINK_ERR_INVALID);
  p.api_base = "https://user@host";
  ASSERT_ERR(aslink_telegram_open(&p, &c), ASLINK_ERR_INVALID);
  p.api_base = "ftp://host";
  ASSERT_ERR(aslink_telegram_open(&p, &c), ASLINK_ERR_INVALID);
  p.api_base = NULL;
  p.token = "123456:abc/../getMe";
  ASSERT_ERR(aslink_telegram_open(&p, &c), ASLINK_ERR_INVALID);
  p.token = ":abc";
  ASSERT_ERR(aslink_telegram_open(&p, &c), ASLINK_ERR_INVALID);
  p.token = "123456:";
  ASSERT_ERR(aslink_telegram_open(&p, &c), ASLINK_ERR_INVALID);
  p.token = NULL;
  ASSERT_ERR(aslink_telegram_open(&p, &c), ASLINK_ERR_INVALID);
  ASSERT_ERR(aslink_telegram_open(NULL, &c), ASLINK_ERR_INVALID);
  aslink_close(NULL);
  aslink_cancel(NULL);
}

TEST(cancel_is_sticky) {
  aslink_telegram_params p;
  aslink_ctx *c = NULL;
  aslink_status st;
  aslink_update *u = NULL;
  size_t n = 0;
  memset(&p, 0, sizeof p);
  p.token = "1:x";
  p.api_base = "http://127.0.0.1:1";
  ASSERT_OK(aslink_telegram_open(&p, &c));
  aslink_cancel(c);
  ASSERT_ERR(aslink_get_updates(c, 0, 0, &u, &n, &st), ASLINK_ERR_CANCELLED);
  ASSERT_ERR(aslink_send_message(c, 5, 0, "x", NULL, NULL, &st), ASLINK_ERR_CANCELLED);
  ASSERT_EQ_STR(st.message, "cancelled");
  aslink_close(c);
}

TEST_LIST = {
  TEST_ENTRY(get_updates_body),
  TEST_ENTRY(send_message_body),
  TEST_ENTRY(send_message_validation),
  TEST_ENTRY(edit_bodies),
  TEST_ENTRY(draft_body),
  TEST_ENTRY(small_method_bodies),
  TEST_ENTRY(length_limits),
  TEST_ENTRY(open_params),
  TEST_ENTRY(cancel_is_sticky),
};

RUN_ALL_TESTS()
