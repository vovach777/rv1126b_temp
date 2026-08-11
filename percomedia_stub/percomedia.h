#pragma once
#include <QObject>
#include <QDebug>
#include <QQueue>
#include <QThread>
#include <QImage>

#include "cb.h"
#include "percomedia_global.h"
#include "percomedia_config.h"

///
/// \brief The PercoMedia class
/// Обязан быть обычным классом С++
/// Предоставляет экспортируемые из библиотеки функции
///
class PERCOMEDIA_EXPORT PercoMedia
{
public:
    PercoMedia();
    ~PercoMedia();
    //void set_facebox_cb(onFaceBox cb);
    void set_personid_cb(onPersonId cb);
    void set_recheck_cb(onPersonId cb);
    void set_lost_cb(onLost cb);
    void set_wdt_timeout_cb(onWDT cb);
    //void set_qual_cb(onQual cb);
    void set_motion_cb(onMotion cb);
    //bool get_motion_st();
    void setFaceCatchRect(QRect r);
    void setFaceCatch(bool st);
    void motion_fake_trig();

    static void signalHandler(int signum);
    QImage face();
    void setNNRun(bool st);
    void resetCurrentUser();
    t_userdata getUserData();

    std::vector<float> getVector();
    t_rect getRect();
    int getId();

    bool setAcc(int id, std::string acc);
    bool setPass(int id, PassState en);

private:

};

