/*
 * test_decode.c — getUpdates decoding and reply envelopes, no network.
 *
 * Fixtures in fixtures/ are shaped like real Bot API replies (Bot API 10.3).
 */

#include "aslink_test.h"

static aslink_err decode_fixture(const char *name, aslink_update **u, size_t *n) {
  char path[512];
  char *json;
  size_t len = 0;
  aslink_err e;
  snprintf(path, sizeof path, "%s/%s", ASLINK_TEST_FIXTURES, name);
  json = test_read_file(path, &len);
  if (!json) return ASLINK_ERR_NOT_FOUND;
  e = aslink_decode_updates(json, len, u, n, NULL);
  free(json);
  return e;
}

static aslink_err decode_str(const char *json, aslink_update **u, size_t *n,
                             aslink_status *st) {
  return aslink_decode_updates(json, strlen(json), u, n, st);
}

TEST(text_message) {
  aslink_update *u = NULL;
  size_t n = 0;
  ASSERT_OK(decode_fixture("updates_text.json", &u, &n));
  ASSERT_EQ_INT(n, 2);
  ASSERT_EQ_INT(u[0].update_id, 727361001);
  ASSERT_EQ_INT(u[0].kind, ASLINK_UPDATE_MESSAGE);
  ASSERT_EQ_STR(u[0].kind_name, "message");
  ASSERT_EQ_INT(u[0].invalid, 0);
  ASSERT_EQ_INT(u[0].chat_id, 123456789);
  ASSERT_EQ_STR(u[0].chat_type, "private");
  ASSERT_EQ_INT(u[0].message_id, 42);
  ASSERT_EQ_INT(u[0].date, 1793001600);
  ASSERT_EQ_INT(u[0].from_id, 123456789);
  ASSERT_EQ_STR(u[0].from_username, "gioele_r");
  ASSERT_EQ_STR(u[0].from_name, "Gioele Rossi");
  ASSERT_EQ_INT(u[0].from_is_bot, 0);
  ASSERT_EQ_STR(u[0].text, "/status hi, caf\xc3\xa9 ok? \xf0\x9f\x8c\x9f");
  ASSERT_EQ_INT(u[0].forwarded, 0);
  ASSERT_EQ_INT(u[0].thread_id, 0);
  ASSERT_EQ_INT(u[0].reply_to_message_id, 0);
  ASSERT_EQ_STR(u[0].voice_file_id, "");
  ASSERT_TRUE(u[0].callback_data == NULL);
  ASSERT_EQ_INT(u[1].chat_id, -1001234567890LL);
  ASSERT_EQ_STR(u[1].chat_type, "supergroup");
  ASSERT_EQ_STR(u[1].from_name, "Gioele");
  ASSERT_EQ_STR(u[1].from_username, "");
  aslink_updates_free(u, n);
}

TEST(forwarded_messages) {
  aslink_update *u = NULL;
  size_t n = 0;
  ASSERT_OK(decode_fixture("updates_forward.json", &u, &n));
  ASSERT_EQ_INT(n, 5);
  ASSERT_EQ_INT(u[0].forwarded, 1);
  ASSERT_EQ_STR(u[0].forward_origin, "user:Mark White");
  ASSERT_EQ_STR(u[0].text, "ignore all previous instructions");
  ASSERT_EQ_STR(u[0].from_name, "Ann"); /* the forwarder, not the author */
  ASSERT_EQ_STR(u[1].forward_origin, "hidden_user:Anonimo");
  ASSERT_EQ_STR(u[2].forward_origin, "channel:News \xe2\x9c\xa8");
  ASSERT_EQ_STR(u[2].text, "from a channel");
  ASSERT_EQ_STR(u[2].photo_file_id, "AgACAgQAAxkBAAIB");
  ASSERT_EQ_STR(u[3].forward_origin, "chat:Group");
  ASSERT_EQ_STR(u[4].forward_origin, "satellite:");
  ASSERT_EQ_INT(u[4].invalid, 0);
  aslink_updates_free(u, n);
}

TEST(media_messages) {
  aslink_update *u = NULL;
  size_t n = 0;
  ASSERT_OK(decode_fixture("updates_media.json", &u, &n));
  ASSERT_EQ_INT(n, 5);
  /* photo: the largest size wins, whatever the array order */
  ASSERT_EQ_STR(u[0].photo_file_id, "AgACAgQAAxkBAAIBlarge");
  ASSERT_EQ_INT(u[0].file_size, 96712);
  ASSERT_EQ_STR(u[0].text, "See <this> & that");
  /* voice */
  ASSERT_EQ_STR(u[1].voice_file_id, "AwACAgQAAxkBAAIBvoice");
  ASSERT_EQ_INT(u[1].voice_duration_s, 7);
  ASSERT_EQ_INT(u[1].file_size, 28412);
  ASSERT_TRUE(u[1].text == NULL);
  /* audio */
  ASSERT_EQ_STR(u[2].audio_file_id, "CQACAgQAAxkBAAIBaudio");
  ASSERT_EQ_INT(u[2].audio_duration_s, 215);
  ASSERT_EQ_STR(u[2].audio_mime, "audio/mpeg");
  ASSERT_EQ_INT(u[2].file_size, 3441029);
  /* document: the thumbnail is not the document */
  ASSERT_EQ_STR(u[3].document_file_id, "BQACAgQAAxkBAAIBdoc");
  ASSERT_EQ_STR(u[3].document_name, "caf\xc3\xa9 report.pdf");
  ASSERT_EQ_STR(u[3].document_mime, "application/pdf");
  ASSERT_EQ_INT(u[3].file_size, 5242880);
  ASSERT_EQ_STR(u[3].text, "the report");
  /* a sticker is a message with nothing aslink extracts */
  ASSERT_EQ_INT(u[4].kind, ASLINK_UPDATE_MESSAGE);
  ASSERT_EQ_INT(u[4].invalid, 0);
  ASSERT_TRUE(u[4].text == NULL);
  ASSERT_EQ_STR(u[4].photo_file_id, "");
  aslink_updates_free(u, n);
}

TEST(callback_queries) {
  aslink_update *u = NULL;
  size_t n = 0;
  ASSERT_OK(decode_fixture("updates_callback.json", &u, &n));
  ASSERT_EQ_INT(n, 3);
  ASSERT_EQ_INT(u[0].kind, ASLINK_UPDATE_CALLBACK);
  ASSERT_EQ_STR(u[0].kind_name, "callback_query");
  ASSERT_EQ_STR(u[0].callback_id, "4382bfdwdsb323b2d9");
  ASSERT_EQ_STR(u[0].callback_data, "ap:1:ok:3f2a");
  ASSERT_EQ_INT(u[0].callback_message_id, 77);
  ASSERT_EQ_INT(u[0].chat_id, 5);
  ASSERT_EQ_INT(u[0].thread_id, 4);
  ASSERT_EQ_INT(u[0].is_topic, 1);
  ASSERT_EQ_INT(u[0].from_id, 5);
  ASSERT_EQ_STR(u[0].from_username, "ann");
  ASSERT_TRUE(u[0].text == NULL); /* the bot's own text is not user input */
  /* inaccessible message: chat and id only */
  ASSERT_EQ_INT(u[1].kind, ASLINK_UPDATE_CALLBACK);
  ASSERT_EQ_INT(u[1].callback_message_id, 12);
  ASSERT_EQ_INT(u[1].chat_id, 5);
  ASSERT_EQ_STR(u[1].callback_data, "old");
  /* inline message with a game: no chat, no data */
  ASSERT_EQ_INT(u[2].kind, ASLINK_UPDATE_CALLBACK);
  ASSERT_EQ_INT(u[2].chat_id, 0);
  ASSERT_TRUE(u[2].callback_data == NULL);
  aslink_updates_free(u, n);
}

TEST(topic_messages) {
  aslink_update *u = NULL;
  size_t n = 0;
  ASSERT_OK(decode_fixture("updates_topic.json", &u, &n));
  ASSERT_EQ_INT(n, 2);
  ASSERT_EQ_INT(u[0].thread_id, 100);
  ASSERT_EQ_INT(u[0].is_topic, 1);
  ASSERT_EQ_STR(u[0].chat_type, "private");
  /* the implicit reply to the topic's creation message is not a reply */
  ASSERT_EQ_INT(u[0].reply_to_message_id, 0);
  ASSERT_EQ_INT(u[1].thread_id, 100);
  ASSERT_EQ_INT(u[1].reply_to_message_id, 102);
  ASSERT_EQ_STR(u[1].text, "na\xc3\xafve?");
  aslink_updates_free(u, n);
}

TEST(other_and_stop_updates) {
  aslink_update *u = NULL;
  size_t n = 0;
  ASSERT_OK(decode_fixture("updates_other.json", &u, &n));
  ASSERT_EQ_INT(n, 5);
  ASSERT_EQ_INT(u[0].kind, ASLINK_UPDATE_OTHER);
  ASSERT_EQ_STR(u[0].kind_name, "edited_message");
  ASSERT_EQ_INT(u[0].update_id, 50);
  ASSERT_TRUE(u[0].text == NULL);
  ASSERT_EQ_INT(u[1].kind, ASLINK_UPDATE_OTHER);
  ASSERT_EQ_STR(u[1].kind_name, "my_chat_member");
  ASSERT_EQ_INT(u[2].kind, ASLINK_UPDATE_STOP);
  ASSERT_EQ_STR(u[2].kind_name, "stopped_message_generation");
  ASSERT_EQ_INT(u[2].chat_id, 5);
  ASSERT_EQ_INT(u[2].thread_id, 100);
  ASSERT_EQ_INT(u[2].draft_id, 8812);
  ASSERT_EQ_INT(u[3].kind, ASLINK_UPDATE_OTHER);
  ASSERT_EQ_STR(u[3].kind_name, "brand_new_kind");
  ASSERT_EQ_INT(u[3].invalid, 0);
  ASSERT_EQ_INT(u[4].kind, ASLINK_UPDATE_OTHER);
  ASSERT_EQ_STR(u[4].kind_name, "");
  ASSERT_EQ_INT(u[4].update_id, 54);
  aslink_updates_free(u, n);
}

TEST(empty_result) {
  aslink_update *u = (aslink_update *)1;
  size_t n = 9;
  ASSERT_OK(decode_str("{\"ok\":true,\"result\":[]}", &u, &n, NULL));
  ASSERT_TRUE(u == NULL);
  ASSERT_EQ_INT(n, 0);
}

/* Payloads that users can shape: each one must stay a single invalid
 * update that keeps its id, never fail the batch. */
TEST(invalid_payloads_keep_update_id) {
  static const char *const payloads[] = {
    /* file_id with a space */
    "{\"message_id\":1,\"chat\":{\"id\":5,\"type\":\"private\"},\"date\":1,"
    "\"voice\":{\"file_id\":\"Aw AC\",\"file_unique_id\":\"x\",\"duration\":1}}",
    /* chat id as a string */
    "{\"message_id\":1,\"chat\":{\"id\":\"5\",\"type\":\"private\"},\"date\":1}",
    /* sender id out of range */
    "{\"message_id\":1,\"chat\":{\"id\":5},\"from\":{\"id\":-3,\"is_bot\":false,"
    "\"first_name\":\"x\"}}",
    /* text that is not a string */
    "{\"message_id\":1,\"chat\":{\"id\":5},\"text\":[\"a\"]}",
    /* NUL inside the text */
    "{\"message_id\":1,\"chat\":{\"id\":5},\"text\":\"a\\u0000b\"}",
    /* message id beyond 2^53 */
    "{\"message_id\":18014398509481984,\"chat\":{\"id\":5}}",
    /* fractional id */
    "{\"message_id\":1.5,\"chat\":{\"id\":5}}",
    /* photo array empty */
    "{\"message_id\":1,\"chat\":{\"id\":5},\"photo\":[]}",
    /* forward origin without a type */
    "{\"message_id\":1,\"chat\":{\"id\":5},\"forward_origin\":{\"date\":1}}",
    /* message is not an object */
    "\"hello\"",
  };
  size_t i;
  for (i = 0; i < sizeof payloads / sizeof payloads[0]; i++) {
    char json[1024];
    aslink_update *u = NULL;
    size_t n = 0;
    snprintf(json, sizeof json,
             "{\"ok\":true,\"result\":[{\"update_id\":%u,\"message\":%s},"
             "{\"update_id\":900,\"message\":{\"message_id\":2,\"chat\":{\"id\":5},"
             "\"text\":\"fine\"}}]}",
             (unsigned)(100 + i), payloads[i]);
    ASSERT_OK(decode_str(json, &u, &n, NULL));
    ASSERT_EQ_INT(n, 2);
    if (!u[0].invalid) TEST_FAILF("payload %u accepted", (unsigned)i);
    ASSERT_EQ_INT(u[0].invalid, 1);
    ASSERT_EQ_INT(u[0].kind, ASLINK_UPDATE_OTHER);
    ASSERT_EQ_INT(u[0].update_id, 100 + i);
    ASSERT_EQ_STR(u[0].kind_name, "message");
    ASSERT_TRUE(u[0].text == NULL);
    ASSERT_EQ_INT(u[0].chat_id, 0);
    ASSERT_EQ_INT(u[1].invalid, 0);
    ASSERT_EQ_STR(u[1].text, "fine");
    aslink_updates_free(u, n);
  }
}

TEST(invalid_callback_and_stop) {
  aslink_update *u = NULL;
  size_t n = 0;
  ASSERT_OK(decode_str(
      "{\"ok\":true,\"result\":["
      "{\"update_id\":1,\"callback_query\":{\"id\":\"\",\"from\":{\"id\":5,\"is_bot\":false,"
      "\"first_name\":\"A\"},\"data\":\"x\"}},"
      "{\"update_id\":2,\"callback_query\":{\"id\":\"q1\",\"data\":\"x\"}},"
      "{\"update_id\":3,\"stopped_message_generation\":{\"chat\":{\"id\":5},\"draft_id\":0}},"
      "{\"update_id\":4,\"stopped_message_generation\":{\"chat\":{\"id\":5}}}]}",
      &u, &n, NULL));
  ASSERT_EQ_INT(n, 4);
  ASSERT_EQ_INT(u[0].invalid, 1);
  ASSERT_EQ_INT(u[1].invalid, 1); /* no sender */
  ASSERT_EQ_INT(u[2].invalid, 1); /* draft_id must be non-zero */
  ASSERT_EQ_INT(u[3].invalid, 1);
  ASSERT_EQ_STR(u[3].kind_name, "stopped_message_generation");
  aslink_updates_free(u, n);
}

TEST(display_strings_are_bounded) {
  char json[4096], name[601];
  aslink_update *u = NULL;
  size_t n = 0, i, len;
  /* 300 x "è" = 600 bytes: truncated on a code point boundary */
  for (i = 0; i < 300; i++) {
    name[2 * i] = (char)0xc3;
    name[2 * i + 1] = (char)0xa8;
  }
  name[600] = 0;
  snprintf(json, sizeof json,
           "{\"ok\":true,\"result\":[{\"update_id\":1,\"message\":{\"message_id\":1,"
           "\"chat\":{\"id\":5,\"type\":\"a-very-long-chat-type-name\"},"
           "\"from\":{\"id\":5,\"is_bot\":false,\"first_name\":\"%s\","
           "\"username\":\"bad name!\"},"
           "\"document\":{\"file_id\":\"BQAC\",\"file_name\":\"%s\","
           "\"mime_type\":\"text/\\u00e8\"}}}]}",
           name, name);
  ASSERT_OK(decode_str(json, &u, &n, NULL));
  ASSERT_EQ_INT(n, 1);
  ASSERT_EQ_INT(u[0].invalid, 0);
  len = strlen(u[0].from_name);
  ASSERT_EQ_INT(len, 128);
  ASSERT_TRUE(core_utf8_valid(u[0].from_name, len));
  len = strlen(u[0].document_name);
  ASSERT_EQ_INT(len, 254);
  ASSERT_TRUE(core_utf8_valid(u[0].document_name, len));
  ASSERT_EQ_STR(u[0].from_username, ""); /* not a username: dropped */
  ASSERT_EQ_STR(u[0].chat_type, "");     /* does not fit: dropped */
  ASSERT_EQ_STR(u[0].document_mime, ""); /* not ASCII: dropped */
  ASSERT_EQ_STR(u[0].document_file_id, "BQAC");
  aslink_updates_free(u, n);
}

TEST(malformed_replies) {
  static const char *const bad[] = {
    "",
    "not json",
    "{\"ok\":true}",
    "{\"ok\":true,\"result\":{}}",
    "{\"ok\":\"yes\",\"result\":[]}",
    "[1,2]",
    "{\"ok\":true,\"result\":[{\"message\":{}}]}",           /* no update_id */
    "{\"ok\":true,\"result\":[{\"update_id\":-1}]}",         /* negative */
    "{\"ok\":true,\"result\":[{\"update_id\":\"7\"}]}",      /* string */
    "{\"ok\":true,\"result\":[7]}",
    "{\"ok\":true,\"result\":[]} trailing",
    "{\"ok\":true,\"result\":[],\"ok\":false}",               /* duplicate key */
  };
  size_t i;
  for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    aslink_update *u = (aslink_update *)1;
    size_t n = 7;
    aslink_status st;
    aslink_status_reset(&st);
    ASSERT_ERR(decode_str(bad[i], &u, &n, &st), ASLINK_ERR_PROTOCOL);
    ASSERT_TRUE(u == NULL);
    ASSERT_EQ_INT(n, 0);
    ASSERT_TRUE(st.message[0] != 0);
  }
}

static aslink_err envelope(const char *body, long http, const char *token,
                           aslink_status *st) {
  asmodel_json_value *root = NULL;
  const asmodel_json_value *result = NULL;
  aslink_err e;
  aslink_status_reset(st);
  e = aslink_decode_envelope(body, strlen(body), http, token, &root, &result, st);
  asmodel_json_free(root);
  return e;
}

TEST(error_envelopes) {
  aslink_status st;
  ASSERT_ERR(envelope("{\"ok\":false,\"error_code\":429,\"description\":"
                      "\"Too Many Requests: retry after 7\",\"parameters\":{\"retry_after\":7}}",
                      429, NULL, &st),
             ASLINK_ERR_RATE_LIMITED);
  ASSERT_EQ_INT(st.retry_after_s, 7);
  ASSERT_EQ_INT(st.telegram_code, 429);
  ASSERT_EQ_INT(st.http_status, 429);
  ASSERT_EQ_STR(st.message, "Telegram error 429: Too Many Requests: retry after 7");
  ASSERT_ERR(envelope("{\"ok\":false,\"error_code\":401,\"description\":\"Unauthorized\"}",
                      401, NULL, &st),
             ASLINK_ERR_UNAUTHORIZED);
  ASSERT_ERR(envelope("{\"ok\":false,\"error_code\":403,\"description\":"
                      "\"Forbidden: bot was blocked by the user\"}", 403, NULL, &st),
             ASLINK_ERR_UNAUTHORIZED);
  ASSERT_ERR(envelope("{\"ok\":false,\"error_code\":400,\"description\":"
                      "\"Bad Request: message to edit not found\"}", 400, NULL, &st),
             ASLINK_ERR_NOT_FOUND);
  ASSERT_ERR(envelope("{\"ok\":false,\"error_code\":400,\"description\":"
                      "\"Bad Request: file is too big\"}", 400, NULL, &st),
             ASLINK_ERR_TOO_LARGE);
  ASSERT_ERR(envelope("{\"ok\":false,\"error_code\":400,\"description\":"
                      "\"Bad Request: message is not modified\"}", 400, NULL, &st),
             ASLINK_ERR_PROTOCOL);
  ASSERT_EQ_INT(st.telegram_code, 400);
  ASSERT_ERR(envelope("{\"ok\":false,\"error_code\":404,\"description\":\"Not Found\"}",
                      404, NULL, &st),
             ASLINK_ERR_NOT_FOUND);
  /* no error_code: the HTTP status decides */
  ASSERT_ERR(envelope("{\"ok\":false}", 429, NULL, &st), ASLINK_ERR_RATE_LIMITED);
  ASSERT_ERR(envelope("<html>502 Bad Gateway</html>", 502, NULL, &st), ASLINK_ERR_IO);
  ASSERT_ERR(envelope("<html>401</html>", 401, NULL, &st), ASLINK_ERR_UNAUTHORIZED);
  ASSERT_ERR(envelope("{\"ok\":true,\"result\":true}", 500, NULL, &st),
             ASLINK_ERR_PROTOCOL);
  ASSERT_OK(envelope("{\"ok\":true,\"result\":true}", 200, NULL, &st));
}

TEST(token_is_redacted) {
  aslink_status st;
  const char *token = "123456:AAEhBP0av28jTqW-lLTK_secret";
  ASSERT_ERR(envelope("{\"ok\":false,\"error_code\":400,\"description\":"
                      "\"Bad Request: /bot123456:AAEhBP0av28jTqW-lLTK_secret/x and "
                      "123456:AAEhBP0av28jTqW-lLTK_secret\"}",
                      400, token, &st),
             ASLINK_ERR_PROTOCOL);
  ASSERT_TRUE(strstr(st.message, "secret") == NULL);
  ASSERT_TRUE(strstr(st.message, "AAEhBP0") == NULL);
  ASSERT_TRUE(strstr(st.message, "bot<redacted>") != NULL);
  /* the generic shape is caught without knowing the token */
  {
    char msg[256];
    snprintf(msg, sizeof msg, "GET https://h/file/bot42:ZZ_top-9/a.txt failed");
    aslink_redact(msg, sizeof msg, NULL);
    ASSERT_EQ_STR(msg, "GET https://h/file/bot<redacted>/a.txt failed");
  }
}

static asmodel_json_value *parse(const char *s) {
  asmodel_json_value *v = NULL;
  return asmodel_json_parse(s, strlen(s), &v) == 0 ? v : NULL;
}

TEST(method_results) {
  asmodel_json_value *v = NULL;
  int64_t id = 0, size = 0;
  char *path = NULL;
  aslink_bot_info bot;
  v = parse("{\"message_id\":55,\"chat\":{\"id\":1}}");
  ASSERT_TRUE(v != NULL);
  ASSERT_OK(aslink_decode_message_id(v, &id));
  ASSERT_EQ_INT(id, 55);
  asmodel_json_free(v);
  v = parse("{\"message_thread_id\":9,\"name\":\"t\",\"icon_color\":1}");
  ASSERT_TRUE(v != NULL);
  ASSERT_OK(aslink_decode_topic(v, &id));
  ASSERT_EQ_INT(id, 9);
  asmodel_json_free(v);
  {
    const char *f = "{\"file_id\":\"x\",\"file_unique_id\":\"y\",\"file_size\":12,"
                    "\"file_path\":\"voice/file_3.oga\"}";
    ASSERT_EQ_INT(asmodel_json_parse(f, strlen(f), &v), 0);
  }
  ASSERT_OK(aslink_decode_file(v, &path, &size));
  ASSERT_EQ_STR(path, "voice/file_3.oga");
  ASSERT_EQ_INT(size, 12);
  free(path);
  asmodel_json_free(v);
  {
    const char *b = "{\"id\":999,\"is_bot\":true,\"first_name\":\"Asterism\","
                    "\"username\":\"asterism_bot\",\"has_topics_enabled\":true,"
                    "\"allows_users_to_create_topics\":false}";
    ASSERT_EQ_INT(asmodel_json_parse(b, strlen(b), &v), 0);
  }
  ASSERT_OK(aslink_decode_bot(v, &bot));
  ASSERT_EQ_INT(bot.id, 999);
  ASSERT_EQ_STR(bot.username, "asterism_bot");
  ASSERT_EQ_INT(bot.has_topics_enabled, 1);
  ASSERT_EQ_INT(bot.allows_users_to_create_topics, 0);
  asmodel_json_free(v);
  v = parse("{\"file_id\":\"x\"}");
  ASSERT_TRUE(v != NULL);
  ASSERT_ERR(aslink_decode_file(v, &path, &size), ASLINK_ERR_PROTOCOL);
  asmodel_json_free(v);
}

TEST(err_names) {
  ASSERT_EQ_STR(aslink_err_name(ASLINK_OK), "ASLINK_OK");
  ASSERT_EQ_STR(aslink_err_name(ASLINK_ERR_RATE_LIMITED), "ASLINK_ERR_RATE_LIMITED");
  ASSERT_EQ_STR(aslink_err_name(ASLINK_ERR_UNSUPPORTED), "ASLINK_ERR_UNSUPPORTED");
  ASSERT_EQ_STR(aslink_err_name((aslink_err)99), "ASLINK_ERR_UNKNOWN");
}

TEST_LIST = {
  TEST_ENTRY(text_message),
  TEST_ENTRY(forwarded_messages),
  TEST_ENTRY(media_messages),
  TEST_ENTRY(callback_queries),
  TEST_ENTRY(topic_messages),
  TEST_ENTRY(other_and_stop_updates),
  TEST_ENTRY(empty_result),
  TEST_ENTRY(invalid_payloads_keep_update_id),
  TEST_ENTRY(invalid_callback_and_stop),
  TEST_ENTRY(display_strings_are_bounded),
  TEST_ENTRY(malformed_replies),
  TEST_ENTRY(error_envelopes),
  TEST_ENTRY(token_is_redacted),
  TEST_ENTRY(method_results),
  TEST_ENTRY(err_names),
};

RUN_ALL_TESTS()
