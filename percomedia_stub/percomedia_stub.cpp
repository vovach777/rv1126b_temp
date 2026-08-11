#include "percomedia.h"
#include "cb.h"

// Заглушка libPM для сборки s30gui на Windows без реальной percomedia.
// Все функции пустые, getters возвращают пустые значения.
// См. doc2_port_plan.md раздел 7, Этап 0.

PercoMedia::PercoMedia() {}
PercoMedia::~PercoMedia() {}

void PercoMedia::set_personid_cb(onPersonId cb) { (void)cb; }
void PercoMedia::set_recheck_cb(onPersonId cb) { (void)cb; }
void PercoMedia::set_lost_cb(onLost cb) { (void)cb; }
void PercoMedia::set_wdt_timeout_cb(onWDT cb) { (void)cb; }
void PercoMedia::set_motion_cb(onMotion cb) { (void)cb; }

void PercoMedia::setFaceCatchRect(QRect r) { (void)r; }
void PercoMedia::setFaceCatch(bool st) { (void)st; }
void PercoMedia::motion_fake_trig() {}

void PercoMedia::signalHandler(int signum) { (void)signum; }

QImage PercoMedia::face() { return QImage(); }
void PercoMedia::setNNRun(bool st) { (void)st; }
void PercoMedia::resetCurrentUser() {}

t_userdata PercoMedia::getUserData() { return t_userdata{}; }
std::vector<float> PercoMedia::getVector() { return {}; }
t_rect PercoMedia::getRect() { return t_rect{0,0,0,0}; }
int PercoMedia::getId() { return -1; }

bool PercoMedia::setAcc(int id, std::string acc) { (void)id; (void)acc; return false; }
bool PercoMedia::setPass(int id, PassState en) { (void)id; (void)en; return false; }
