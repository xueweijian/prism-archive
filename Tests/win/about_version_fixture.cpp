// PrismArchive -- card C57 test fixture.
//
// This executable exists for ONE reason: it carries a VERSIONINFO block whose
// version (9.8.7.6) is not the product's (0.1.0.0). The About dialog is pointed
// at it, and the report in the text control must show the FIXTURE's number.
// A dialog that hardcodes the version therefore fails the suite -- which is the
// negative control PRISM_C57_MUTANT_HARDCODED_VERSION exists to prove.
//
// It is never installed, never shipped and does nothing at runtime.
int main() { return 0; }
