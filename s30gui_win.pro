# Windows сборка s30gui без ROCKCHIP (режим ПК, симуляция)
# Копия s30gui.pro с адаптацией для Windows + заглушка libPM
# См. doc2_port_plan.md раздел 7, Этап 0

TEMPLATE = app
QT += core gui widgets scxml websockets svg
# core-private убран — не используется (grep: 0 private includes)

CONFIG += console silent

QMAKE_CFLAGS_WARN_ON += -Wno-class-memaccess
QMAKE_CXXFLAGS_WARN_ON += -Wno-unused-parameter
QMAKE_CFLAGS += -Wno-class-memaccess
QMAKE_CXXFLAGS += -Wno-class-memaccess
QMAKE_CXXFLAGS += -Og

DEFINES += GIT_CURRENT_SHA1="\\\"win-stub\\\""

# Заглушка libPM (собрана отдельно, лежит в percomedia_stub/release/)
PERCOMEDIA_DIR = K:/rv1126b_temp/percomedia_stub
LIBS += -L$$PERCOMEDIA_DIR/release/ -lpercomedia
INCLUDEPATH += $$PERCOMEDIA_DIR
DEPENDPATH += $$PERCOMEDIA_DIR

SOURCES += \
    aiservice.cpp \
    components/datasrc.cpp \
    components/passbar.cpp \
    conf.cpp \
    dbg/dbg.cpp \
    main.cpp \
    mainwin.cpp \
    perco.cpp \
    components/screensaver.cpp \
    scenes/fragment.cpp \
    types/cardsinfo.cpp \
    types/eventprofile.cpp \
    types/jopcodes.cpp \
    types/jroles.cpp \
    types/settings.cpp \
    types/tmessage.cpp \
    types/userprofile.cpp \
    utils/jsoncomparer.cpp \
    ws/QTlsServer.cpp \
    ws/QWsFrame.cpp \
    ws/QWsHandshake.cpp \
    ws/QWsServer.cpp \
    ws/QWsSocket.cpp \
    ws/functions.cpp

HEADERS += \
    aiservice.h \
    cardeventssource.h \
    components/barclock.h \
    components/calendar.h \
    components/cardreadermsgbox.h \
    components/datasrc.h \
    components/digital_kbd.h \
    components/esearchpanel.h \
    components/eselectpanel.h \
    components/eventlist.h \
    components/faceitem.h \
    components/facescanitem.h \
    components/factory.h \
    components/gbtntxt.h \
    components/gkbd.h \
    components/glabel.h \
    components/glist.h \
    components/gscrollabel.h \
    components/gslider.h \
    components/interval.h \
    components/ipchanger.h \
    components/mfilter.h \
    components/pageitem.h \
    components/passbar.h \
    components/passchanger.h \
    components/passedit.h \
    components/pixbtn.h \
    components/propertybar.h \
    components/svgbtn.h \
    components/toppanel.h \
    components/usearchpanel.h \
    components/usrlist.h \
    delegates/geditors.h \
    devicesrc.h \
    eventlistsrc.h \
    exdevsrc.h \
    json.hpp \
    components/passwordanimation.h \
    components/popupitem.h \
    components/popupanimation.h \
    components/shortdelegate.h \
    conf.h \
    dbg/dbg.h \
    delegates/name_view.h \
    delegates/namevalue_view.h \
    delegates/property_view.h \
    delegates/user_shortview.h \
    delegates/value_view.h \
    delegates/wrappers.h \
    jsonservice.h \
    mainwin.h \
    msgsource.h \
    oldsettings.h \
    perco.h \
    pinsrc.h \
    replacesrc.h \
    scenes/saddcard.h \
    scenes/sadduser.h \
    scenes/scardsearch.h \
    scenes/schangepass.h \
    scenes/sclock.h \
    scenes/sdatetime.h \
    scenes/sdevice.h \
    scenes/seditor.h \
    scenes/sedituser.h \
    scenes/seventlist.h \
    scenes/sexdev.h \
    scenes/sfacereg.h \
    scenes/sfaceremotecatch.h \
    scenes/sfacesearch.h \
    scenes/sinfo.h \
    scenes/sinputs.h \
    scenes/smenu.h \
    scenes/snet.h \
    scenes/soutputs.h \
    scenes/spassword.h \
    scenes/sscreen.h \
    scenes/ssettings.h \
    scenes/sterminal.h \
    scenes/suserlisrt.h \
    scenes/suserselect.h \
    scenes/svideo.h \
    scenes/fragment.h \
    components/screensaver.h \
    touchservice.h \
    types/cardsinfo.h \
    types/context.h \
    types/enums.h \
    types/eventprofile.h \
    types/jopcodes.h \
    types/jroles.h \
    types/params.h \
    types/settings.h \
    types/tmessage.h \
    types/userprofile.h \
    userlistsrc.h \
    utils/ijclient.h \
    utils/ijserver.h \
    utils/jsoncomparer.h \
    ws/QTlsServer.h \
    ws/QWsFrame.h \
    ws/QWsHandshake.h \
    ws/QWsServer.h \
    ws/QWsSocket.h \
    ws/WsEnums.h \
    ws/functions.h

FORMS +=

RESOURCES += \
    resource.qrc

STATECHARTS += \
    MenuState.scxml

TRANSLATIONS += QtLanguage_ru.ts
CODECFORSRC     = UTF-8

DISTFILES += \
    kbd/numkbd.png \
    kbd/numkbd.svg
