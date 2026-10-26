/*
 * test_names.c — section and source names.
 */

#include "asper_test.h"

#include "asper_internal.h"

TEST(section_source_names) {
  asper_section s;
  asper_source src;
  ASSERT_EQ_STR(asper_section_name(ASPER_SECTION_IDENTITY), "identity");
  ASSERT_EQ_STR(asper_section_name(ASPER_SECTION_CONTEXT), "context");
  ASSERT_EQ_STR(asper_section_name(ASPER_SECTION_PROJECT), "project");
  ASSERT_TRUE(asper_section_parse("identity", &s));
  ASSERT_EQ_INT(s, ASPER_SECTION_IDENTITY);
  ASSERT_TRUE(asper_section_parse("project", &s));
  ASSERT_EQ_INT(s, ASPER_SECTION_PROJECT);
  ASSERT_TRUE(!asper_section_parse("sideways", &s));
  ASSERT_EQ_STR(asper_source_name(ASPER_SRC_SEED), "seed");
  ASSERT_EQ_STR(asper_source_name(ASPER_SRC_MANUAL), "manual");
  ASSERT_EQ_STR(asper_source_name(ASPER_SRC_CURATOR), "curator");
  ASSERT_TRUE(asper_source_parse("curator", &src));
  ASSERT_EQ_INT(src, ASPER_SRC_CURATOR);
  ASSERT_TRUE(!asper_source_parse("oracle", &src));
}

TEST_LIST = {
    TEST_ENTRY(section_source_names),
};

RUN_ALL_TESTS()
