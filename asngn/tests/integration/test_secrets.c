/* Secrets at the tool boundary: the tool receives the value, every record
 * and model prompt keeps the placeholder, and a tool outside the secret's
 * policy is refused before dispatch. */
#include "asngn_test.h"
#include "asvault.h"
#include "engine_fx.h"
#include "asngn_internal.h"

#define SECRET "s3cr3t-value-0123456789"

static int store_secret(eng_fx *f, const char *tool) {
  char dir[600];
  asvault *v = NULL;
  const char *tools[1] = {tool};
  asvault_policy policy = {tools, 1, NULL, 0};
  snprintf(dir, sizeof dir, "%s/vault", f->root_raw);
  int ok = asvault_open(dir, &v) == ASVAULT_OK && asvault_set(v, "api", SECRET, &policy) == ASVAULT_OK;
  asvault_close(v);
  return ok;
}

static int script_call(eng_fx *f) {
  return fake_model_push(&f->nano, "CLASS MODERATE | DETAIL NORMAL | MODE PLAN | TASK LOOKUP\n") &&
         fake_model_push(&f->light,
                         "{action:\"call\",why:\"use the key\",input:fake.mut {msg:\"key={{secret:api}}\"},"
                         "success:\"observed\",fallback:\"report\"}\n") &&
         fake_model_push(&f->light, "{action:\"answer\"}\n") &&
         fake_model_push(&f->stdm, "Done.\n");
}

TEST(secret_reaches_the_tool_but_never_the_model) {
  eng_fx f;
  ASSERT_TRUE(eng_setup(&f, "echo", "safety: { autoconfirm: \"allow\" },"));
  ASSERT_TRUE(store_secret(&f, "fake"));
  ASSERT_TRUE(script_call(&f));
  asngn_turn_result r = {0};
  ASSERT_OK(eng_turn(&f, "Call the API", NULL, NULL, &r));
  asngn_stats stats;
  ASSERT_OK(asngn_get_stats(f.c, &stats));
  ASSERT_EQ_INT(stats.tool_calls, 1);
  /* The echo tool returned the resolved arguments; the model saw them redacted. */
  ASSERT_TRUE(f.light.last_user != NULL);
  ASSERT_TRUE(strstr(f.light.last_user, SECRET) == NULL);
  ASSERT_CONTAINS(f.light.last_user, "{{secret:api}}");
  ASSERT_TRUE(f.stdm.last_user == NULL || strstr(f.stdm.last_user, SECRET) == NULL);
  /* What the tool process received: the value, for this tool only. */
  char *resolved = NULL, why[256];
  ASSERT_OK(asngn_secrets_resolve(f.c, "fake@1.0.0", "{msg: \"key={{secret:api}}\"}", &resolved,
                                  why, sizeof why));
  ASSERT_CONTAINS(resolved, "key=" SECRET);
  asngn_secrets_free(resolved);
  ASSERT_ERR(asngn_secrets_resolve(f.c, "grep@1.0.0", "{q: \"{{secret:api}}\"}", &resolved, why,
                                   sizeof why), ASNGN_ERR_DENIED);
  /* And what came back is redacted, also when the tool re-encodes it. */
  char *echoed = core_strdup("token " SECRET " end");
  asngn_secrets_redact(f.c, &echoed);
  ASSERT_EQ_STR(echoed, "token {{secret:api}} end");
  free(echoed);
  asngn_turn_result_free(&r);
  eng_drop(&f);
}

TEST(secret_outside_its_policy_is_refused_before_dispatch) {
  eng_fx f;
  ASSERT_TRUE(eng_setup(&f, "echo", "safety: { autoconfirm: \"allow\" },"));
  ASSERT_TRUE(store_secret(&f, "mail"));
  ASSERT_TRUE(script_call(&f));
  asngn_turn_result r = {0};
  ASSERT_OK(eng_turn(&f, "Call the API", NULL, NULL, &r));
  asngn_stats stats;
  ASSERT_OK(asngn_get_stats(f.c, &stats));
  ASSERT_EQ_INT(stats.tool_calls, 0);
  ASSERT_CONTAINS(f.light.last_user, "asvault/denied");
  ASSERT_TRUE(strstr(f.light.last_user, SECRET) == NULL);
  asngn_turn_result_free(&r);
  eng_drop(&f);
}

TEST_LIST = {
  TEST_ENTRY(secret_reaches_the_tool_but_never_the_model),
  TEST_ENTRY(secret_outside_its_policy_is_refused_before_dispatch),
};
RUN_ALL_TESTS()
