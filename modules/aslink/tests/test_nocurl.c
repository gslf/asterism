/*
 * test_nocurl.c — aslink built without a transport (as on a host with
 * neither libcurl nor WinHTTP): every network call reports UNSUPPORTED
 * after validating its arguments, nothing touches the filesystem, and the
 * helpers still work.
 */

#include "aslink_test.h"

TEST(network_calls_are_unsupported) {
  aslink_telegram_params p;
  aslink_ctx *c = NULL;
  aslink_status st;
  aslink_update *u = NULL;
  aslink_bot_info bot;
  size_t n = 0;
  int64_t id = -1;
  char *body = (char *)1, dir[256], dest[300];
  static const aslink_command cmd = {"status", "Status"};
  ASSERT_EQ_INT(aslink_has_transport(), 0);
  memset(&p, 0, sizeof p);
  p.token = "123456:AAEhBP0av28jTqW";
  ASSERT_OK(aslink_telegram_open(&p, &c));
  ASSERT_ERR(aslink_get_updates(c, 0, 30, &u, &n, &st), ASLINK_ERR_UNSUPPORTED);
  ASSERT_TRUE(strstr(st.message, "libcurl") != NULL);
  ASSERT_ERR(aslink_send_message(c, 5, 0, "x", NULL, &id, &st), ASLINK_ERR_UNSUPPORTED);
  ASSERT_EQ_INT(id, 0);
  ASSERT_ERR(aslink_edit_message(c, 5, 1, "x", NULL, &st), ASLINK_ERR_UNSUPPORTED);
  ASSERT_ERR(aslink_edit_keyboard(c, 5, 1, NULL, &st), ASLINK_ERR_UNSUPPORTED);
  ASSERT_ERR(aslink_send_draft(c, 5, 0, 1, "x", NULL, &st), ASLINK_ERR_UNSUPPORTED);
  ASSERT_ERR(aslink_send_chat_action(c, 5, 0, "typing", &st), ASLINK_ERR_UNSUPPORTED);
  ASSERT_ERR(aslink_answer_callback(c, "q", NULL, 0, &st), ASLINK_ERR_UNSUPPORTED);
  ASSERT_ERR(aslink_create_topic(c, 5, "t", &id, &st), ASLINK_ERR_UNSUPPORTED);
  ASSERT_ERR(aslink_set_commands(c, &cmd, 1, &st), ASLINK_ERR_UNSUPPORTED);
  ASSERT_ERR(aslink_get_me(c, &bot, &st), ASLINK_ERR_UNSUPPORTED);
  ASSERT_TRUE(test_tmpdir(dir));
  snprintf(dest, sizeof dest, "%s/out.bin", dir);
  ASSERT_ERR(aslink_download_file(c, "BQAC", dest, 0, &st), ASLINK_ERR_UNSUPPORTED);
  ASSERT_EQ_INT(os_file_exists(dest), 0);
  test_rmtree(dir);
  /* validation still comes first */
  ASSERT_ERR(aslink_send_message(c, 0, 0, "x", NULL, NULL, &st), ASLINK_ERR_INVALID);
  ASSERT_ERR(aslink_send_document(c, 5, 0, "/nonexistent/file", NULL, &st),
             ASLINK_ERR_NOT_FOUND);
  ASSERT_ERR(aslink_http_get("https://example.org/", 1024, 0, &body, NULL, NULL),
             ASLINK_ERR_UNSUPPORTED);
  ASSERT_TRUE(body == NULL);
  ASSERT_ERR(aslink_http_get("file:///etc/passwd", 1024, 0, &body, NULL, NULL),
             ASLINK_ERR_INVALID);
  aslink_close(c);
}

TEST(helpers_still_work) {
  char **parts = NULL;
  size_t n = 0;
  char *s = aslink_html_escape("a<b");
  aslink_update *u = NULL;
  const char *json = "{\"ok\":true,\"result\":[{\"update_id\":1,\"message\":{\"message_id\":1,"
                     "\"chat\":{\"id\":5,\"type\":\"private\"},\"text\":\"hi\"}}]}";
  ASSERT_EQ_STR(s, "a&lt;b");
  free(s);
  ASSERT_OK(aslink_split_text("one two three", 5, &parts, &n));
  ASSERT_EQ_INT(n, 3);
  aslink_strings_free(parts, n);
  ASSERT_OK(aslink_decode_updates(json, strlen(json), &u, &n, NULL));
  ASSERT_EQ_INT(n, 1);
  ASSERT_EQ_STR(u[0].text, "hi");
  aslink_updates_free(u, n);
}

TEST_LIST = {
  TEST_ENTRY(network_calls_are_unsupported),
  TEST_ENTRY(helpers_still_work),
};

RUN_ALL_TESTS()
