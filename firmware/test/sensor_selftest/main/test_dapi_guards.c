#include "selftest.h"

#include "dapi_guards.h"

void test_dapi_guards(void)
{
    /* the thingsboard source may not forget the network the board is on */
    CHECK(dapi_wifi_forget_blocked(DAPI_SRC_TB, "farm", "farm") == true);
    CHECK(dapi_wifi_forget_blocked(DAPI_SRC_TB, "other", "farm") == false);
    /* the local web app keeps today's behaviour */
    CHECK(dapi_wifi_forget_blocked(DAPI_SRC_HTTP, "farm", "farm") == false);
    /* not connected: nothing is in use */
    CHECK(dapi_wifi_forget_blocked(DAPI_SRC_TB, "farm", "") == false);
    CHECK(dapi_wifi_forget_blocked(DAPI_SRC_TB, "", "") == false);
    CHECK(dapi_wifi_forget_blocked(DAPI_SRC_TB, "farm", NULL) == false);
    CHECK(dapi_wifi_forget_blocked(DAPI_SRC_TB, NULL, "farm") == false);
    /* names are compared exactly */
    CHECK(dapi_wifi_forget_blocked(DAPI_SRC_TB, "Farm", "farm") == false);
    CHECK(dapi_wifi_forget_blocked(DAPI_SRC_TB, "farm2", "farm") == false);
}
