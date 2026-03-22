#include "unity.h"
#include "grocy_client.h"
#include <stdlib.h>
#include <string.h>

/* ── Sample JSON fixtures matching the Grocy API response format ── */

/* Nested product object format (modern Grocy) */
static const char GROCY_RESPONSE_NESTED[] =
    "["
    "  {"
    "    \"product_id\": 42,"
    "    \"amount\": 3.0,"
    "    \"product\": {"
    "      \"name\": \"Milk\","
    "      \"picture_file_name\": \"milk.jpg\""
    "    }"
    "  },"
    "  {"
    "    \"product_id\": 7,"
    "    \"amount\": 0.5,"
    "    \"product\": {"
    "      \"name\": \"Apples\","
    "      \"picture_file_name\": \"\""
    "    }"
    "  }"
    "]";

/* Flat format (older Grocy or some endpoints) */
static const char GROCY_RESPONSE_FLAT[] =
    "["
    "  {"
    "    \"product_id\": 1,"
    "    \"amount\": 10.0,"
    "    \"product_name\": \"Bread\","
    "    \"picture_file_name\": \"bread.png\""
    "  }"
    "]";

static const char GROCY_RESPONSE_EMPTY[] = "[]";

static const char GROCY_RESPONSE_MISSING_FIELDS[] =
    "["
    "  { \"product_id\": 99, \"product\": { \"name\": \"NoImage\" } },"
    "  { \"amount\": 1.0 }"   /* missing product_id — should be skipped */
    "]";

/* Sort order fixture: unsorted names that should come out alphabetically */
static const char GROCY_RESPONSE_UNSORTED[] =
    "["
    "  { \"product_id\": 3, \"amount\": 1, \"product\": { \"name\": \"Zucchini\" } },"
    "  { \"product_id\": 1, \"amount\": 1, \"product\": { \"name\": \"Apple\" } },"
    "  { \"product_id\": 2, \"amount\": 1, \"product\": { \"name\": \"Mango\" } }"
    "]";

/* ── Helper ── */
static void free_list(grocy_product_list_msg_t *list)
{
    free(list->products);
    list->products = NULL;
    list->count    = 0;
}

/* ── Tests ── */

TEST_CASE("parse nested product format", "[grocy_json]")
{
    grocy_product_list_msg_t list = {0};
    esp_err_t ret = grocy_parse_product_list_json(
        (const uint8_t *)GROCY_RESPONSE_NESTED,
        sizeof(GROCY_RESPONSE_NESTED) - 1, &list);

    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_NOT_NULL(list.products);
    TEST_ASSERT_EQUAL_UINT16(2, list.count);

    /* After alphabetical sort: Apples (id=7), Milk (id=42) */
    TEST_ASSERT_EQUAL_UINT32(7,  list.products[0].id);
    TEST_ASSERT_EQUAL_STRING("Apples", list.products[0].name);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.5f, list.products[0].stock_amount);

    TEST_ASSERT_EQUAL_UINT32(42, list.products[1].id);
    TEST_ASSERT_EQUAL_STRING("Milk", list.products[1].name);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 3.0f, list.products[1].stock_amount);
    TEST_ASSERT_EQUAL_STRING("milk.jpg", list.products[1].picture_filename);

    free_list(&list);
}

TEST_CASE("parse flat product format", "[grocy_json]")
{
    grocy_product_list_msg_t list = {0};
    esp_err_t ret = grocy_parse_product_list_json(
        (const uint8_t *)GROCY_RESPONSE_FLAT,
        sizeof(GROCY_RESPONSE_FLAT) - 1, &list);

    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_EQUAL_UINT16(1, list.count);
    TEST_ASSERT_EQUAL_STRING("Bread", list.products[0].name);
    TEST_ASSERT_EQUAL_UINT32(1, list.products[0].id);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, list.products[0].stock_amount);
    TEST_ASSERT_EQUAL_STRING("bread.png", list.products[0].picture_filename);

    free_list(&list);
}

TEST_CASE("empty product list returns count 0", "[grocy_json]")
{
    grocy_product_list_msg_t list = {0};
    esp_err_t ret = grocy_parse_product_list_json(
        (const uint8_t *)GROCY_RESPONSE_EMPTY,
        sizeof(GROCY_RESPONSE_EMPTY) - 1, &list);

    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_EQUAL_UINT16(0, list.count);

    free_list(&list);
}

TEST_CASE("entry missing product_id is skipped", "[grocy_json]")
{
    grocy_product_list_msg_t list = {0};
    esp_err_t ret = grocy_parse_product_list_json(
        (const uint8_t *)GROCY_RESPONSE_MISSING_FIELDS,
        sizeof(GROCY_RESPONSE_MISSING_FIELDS) - 1, &list);

    TEST_ASSERT_EQUAL(ESP_OK, ret);
    /* Only 1 valid entry (the one with both product_id and product) */
    TEST_ASSERT_EQUAL_UINT16(1, list.count);
    TEST_ASSERT_EQUAL_UINT32(99, list.products[0].id);
    TEST_ASSERT_EQUAL_STRING("NoImage", list.products[0].name);
    /* No picture_file_name in this fixture → should remain empty */
    TEST_ASSERT_EQUAL_STRING("", list.products[0].picture_filename);

    free_list(&list);
}

TEST_CASE("products are sorted alphabetically (case-insensitive)", "[grocy_json]")
{
    grocy_product_list_msg_t list = {0};
    esp_err_t ret = grocy_parse_product_list_json(
        (const uint8_t *)GROCY_RESPONSE_UNSORTED,
        sizeof(GROCY_RESPONSE_UNSORTED) - 1, &list);

    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_EQUAL_UINT16(3, list.count);
    TEST_ASSERT_EQUAL_STRING("Apple",    list.products[0].name);
    TEST_ASSERT_EQUAL_STRING("Mango",    list.products[1].name);
    TEST_ASSERT_EQUAL_STRING("Zucchini", list.products[2].name);

    free_list(&list);
}

TEST_CASE("invalid JSON returns ESP_FAIL", "[grocy_json]")
{
    const char *bad_json = "{ not valid json [[[";
    grocy_product_list_msg_t list = {0};
    esp_err_t ret = grocy_parse_product_list_json(
        (const uint8_t *)bad_json, strlen(bad_json), &list);

    TEST_ASSERT_EQUAL(ESP_FAIL, ret);
}

TEST_CASE("zero stock_amount when amount field absent", "[grocy_json]")
{
    const char *json =
        "[{\"product_id\":5,\"product\":{\"name\":\"NoStock\"}}]";
    grocy_product_list_msg_t list = {0};
    esp_err_t ret = grocy_parse_product_list_json(
        (const uint8_t *)json, strlen(json), &list);

    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_EQUAL_UINT16(1, list.count);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, list.products[0].stock_amount);

    free_list(&list);
}
