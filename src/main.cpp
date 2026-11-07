/* SPDX-License-Identifier: Apache-2.0 */

#include "composition.hpp"


extern "C" int main(void) {
    static app::Composition composition;
    if (composition.Init() != 0) {
        return 0;
    }
    composition.Run();
    return 0;
}
