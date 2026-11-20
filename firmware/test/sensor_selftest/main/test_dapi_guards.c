#include "selftest.h"

#include <string.h>

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

    /* setauto from thingsboard: only the connected network's own profile */
    CHECK(dapi_wifi_setauto_blocked(DAPI_SRC_TB, 2, 2) == false);
    CHECK(dapi_wifi_setauto_blocked(DAPI_SRC_TB, 0, 0) == false);
    CHECK(dapi_wifi_setauto_blocked(DAPI_SRC_TB, -1, 2) == true);  /* clear */
    CHECK(dapi_wifi_setauto_blocked(DAPI_SRC_TB, -1, -1) == true); /* clear, not connected */
    CHECK(dapi_wifi_setauto_blocked(DAPI_SRC_TB, 1, 2) == true);   /* another profile */
    CHECK(dapi_wifi_setauto_blocked(DAPI_SRC_TB, 0, -1) == true);  /* not connected: cannot prove */
    CHECK(dapi_wifi_setauto_blocked(DAPI_SRC_TB, 99, -1) == true); /* out of range */
    CHECK(dapi_wifi_setauto_blocked(DAPI_SRC_HTTP, -1, -1) == false);
    CHECK(dapi_wifi_setauto_blocked(DAPI_SRC_HTTP, 1, 2) == false);

    /* save from thingsboard may not change the active station password */
    CHECK(dapi_wifi_save_password_blocked(DAPI_SRC_TB, "newpass", "oldpass") == true);
    CHECK(dapi_wifi_save_password_blocked(DAPI_SRC_TB, "same", "same") == false);
    CHECK(dapi_wifi_save_password_blocked(DAPI_SRC_TB, "", "oldpass") == false); /* keeps the saved one */
    CHECK(dapi_wifi_save_password_blocked(DAPI_SRC_TB, NULL, "oldpass") == false);
    CHECK(dapi_wifi_save_password_blocked(DAPI_SRC_TB, "x", NULL) == true);      /* cannot read it */
    CHECK(dapi_wifi_save_password_blocked(DAPI_SRC_TB, "", NULL) == false);
    CHECK(dapi_wifi_save_password_blocked(DAPI_SRC_TB, "Same", "same") == true);
    CHECK(dapi_wifi_save_password_blocked(DAPI_SRC_HTTP, "newpass", "oldpass") == false);

    /* relay polarity from thingsboard: only equal to the stored value */
    CHECK(dapi_polarity_write_blocked(DAPI_SRC_TB, "active_low", "active_high") == true);
    CHECK(dapi_polarity_write_blocked(DAPI_SRC_TB, "active_high", "active_high") == false);
    CHECK(dapi_polarity_write_blocked(DAPI_SRC_TB, "bogus", "active_high") == false); /* not a polarity: the parser rejects it with its own error */
    CHECK(dapi_polarity_write_blocked(DAPI_SRC_TB, "", "active_high") == false);
    CHECK(dapi_polarity_write_blocked(DAPI_SRC_TB, NULL, "active_high") == false); /* key absent */
    CHECK(dapi_polarity_write_blocked(DAPI_SRC_HTTP, "active_low", "active_high") == false);

    /* remote wifi.connect guards */
    CHECK(dapi_wifi_connect_refusal(DAPI_SRC_HTTP, true, true, true) == NULL);
    CHECK(dapi_wifi_connect_refusal(DAPI_SRC_TB, false, false, false) == NULL);
    CHECK(strcmp(dapi_wifi_connect_refusal(DAPI_SRC_TB, true, true, true), "local_only") == 0);
    CHECK(strcmp(dapi_wifi_connect_refusal(DAPI_SRC_TB, false, true, true), "staging_active") == 0);
    CHECK(strcmp(dapi_wifi_connect_refusal(DAPI_SRC_TB, false, false, true), "measure_running") == 0);

    /* remote save of a new network needs a password */
    CHECK(dapi_wifi_save_empty_blocked(DAPI_SRC_HTTP, "", false) == false);
    CHECK(dapi_wifi_save_empty_blocked(DAPI_SRC_TB, "", false) == true);
    CHECK(dapi_wifi_save_empty_blocked(DAPI_SRC_TB, NULL, false) == true);
    CHECK(dapi_wifi_save_empty_blocked(DAPI_SRC_TB, "", true) == false);
    CHECK(dapi_wifi_save_empty_blocked(DAPI_SRC_TB, "pw12345678", false) == false);
}
