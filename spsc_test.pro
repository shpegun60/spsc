TEMPLATE = subdirs
CONFIG += ordered

SUBDIRS += \
    test_shadow_off \
    test_shadow_on \
    test_shadow_heur \
    test_cxx20_span \
    launcher

test_shadow_off.file = qmake/test_shadow_off.pro
test_shadow_on.file = qmake/test_shadow_on.pro
test_shadow_heur.file = qmake/test_shadow_heur.pro
test_cxx20_span.file = qmake/test_cxx20_span.pro
launcher.file = qmake/launcher.pro
launcher.depends = test_shadow_off test_shadow_on test_shadow_heur
