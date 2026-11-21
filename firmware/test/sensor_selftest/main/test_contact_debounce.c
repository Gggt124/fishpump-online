#include "selftest.h"
#include "contact_debounce.h"

void test_contact_debounce(void)
{
    contact_debounce_t d;
    contact_debounce_init(&d, false, 0);
    CHECK(!contact_debounce_update(&d, true, 100, 1000) && !d.state);   /* candidate starts */
    CHECK(!contact_debounce_update(&d, true, 1099, 1000) && !d.state);  /* 999 ms: not yet */
    CHECK(contact_debounce_update(&d, true, 1100, 1000) && d.state);    /* 1000 ms: accepted once */
    CHECK(!contact_debounce_update(&d, true, 1200, 1000));              /* no repeat */
    CHECK(!contact_debounce_update(&d, false, 1300, 1000));             /* chatter ... */
    CHECK(!contact_debounce_update(&d, true, 1700, 1000) && d.state);   /* ... shorter than 1 s is ignored */
    CHECK(!contact_debounce_update(&d, false, 2000, 1000));
    CHECK(contact_debounce_update(&d, false, 3000, 1000) && !d.state);  /* other direction */
    contact_debounce_init(&d, false, 0xFFFFFF00u);                      /* across the uint32 wrap */
    contact_debounce_update(&d, true, 0xFFFFFF80u, 1000);
    CHECK(contact_debounce_update(&d, true, 0x00000380u, 1000) && d.state);
}
