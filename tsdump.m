// tsdump: list a class's methods and type encodings from the TimeSync framework.
// This is how libgptp was written, and how to fix it when a macOS release
// changes the framework: gptp_open names the missing selector, tsdump shows
// what the class has instead.
//   ./tsdump TSgPTPClock TSgPTPEthernetPort
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
static void dump(const char *name) {
    Class c = objc_getClass(name);
    if (!c) { printf("== %s: not found\n", name); return; }
    printf("== %s (super %s)\n", name, class_getName(class_getSuperclass(c)));
    unsigned n; Method *m;
    m = class_copyMethodList(object_getClass(c), &n);
    for (unsigned i = 0; i < n; i++) printf("  + %s  %s\n", sel_getName(method_getName(m[i])), method_getTypeEncoding(m[i]));
    free(m);
    m = class_copyMethodList(c, &n);
    for (unsigned i = 0; i < n; i++) printf("  - %s  %s\n", sel_getName(method_getName(m[i])), method_getTypeEncoding(m[i]));
    free(m);
    objc_property_t *p = class_copyPropertyList(c, &n);
    for (unsigned i = 0; i < n; i++) printf("  @ %s  %s\n", property_getName(p[i]), property_getAttributes(p[i]));
    free(p);
}
int main(int argc, char **argv) {
    void *h = dlopen("/System/Library/PrivateFrameworks/TimeSync.framework/TimeSync", RTLD_NOW);
    if (!h) { printf("dlopen failed: %s\n", dlerror()); return 1; }
    for (int i = 1; i < argc; i++) dump(argv[i]);
    return 0;
}
