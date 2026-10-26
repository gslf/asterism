/* The jail mode as its own executable, for tests that cannot run asterism. */
#include "astools.h"

int main(int argc, char **argv) { return astools_jail_main(argc, argv); }
