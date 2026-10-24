#include "update_version.h"

#include <iostream>

int main() {
    using snaplite::CompareReleaseVersions;
    if (CompareReleaseVersions(L"v1.7.0", L"1.6.9") != 1) return 1;
    if (CompareReleaseVersions(L"1.10.0", L"v1.9.9") != 1) return 2;
    if (CompareReleaseVersions(L"v1.7.0", L"1.7.0") != 0) return 3;
    if (CompareReleaseVersions(L"1.6.9", L"1.7.0") != -1) return 4;
    if (CompareReleaseVersions(L"v1.07.0", L"1.7.0")) return 5;
    if (CompareReleaseVersions(L"latest", L"1.7.0")) return 6;
    std::cout << "Release version comparison passed.\n";
    return 0;
}
