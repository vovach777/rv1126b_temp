#pragma once
#include <vector>
#include <QObject>
#include <string>
typedef struct {
    int x;
    int y;
} t_point;
Q_DECLARE_METATYPE(t_point)
typedef struct {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
} t_rect;
Q_DECLARE_METATYPE(t_rect)
Q_DECLARE_METATYPE(std::string) //иначе молча умрет queue в sig/slot!!!
Q_DECLARE_METATYPE(std::vector<float>)
typedef struct {
    int track_id = -1;
    std::string acct = "";
    std::string photo = "";
    t_rect r = {0,0,0,0};
    std::vector<float> face;
} t_userdata;
Q_DECLARE_METATYPE(t_userdata)

typedef void (*onFaceBox)(t_rect r,std::vector<t_point> p);
typedef void (*onQual)(float ir_blur,float rgb_blur, float qual, int size);
typedef void (*onPersonId)(t_userdata d);
typedef void (*onLost)();
typedef void (*onWDT)();
typedef void (*onMotion)(bool st);
