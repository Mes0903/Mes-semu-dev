#include <stdbool.h>

#include "utils.h"


int main(void)
{
    semu_boot_complete_store(false);
    return semu_boot_complete_load() ? 1 : 0;
}
