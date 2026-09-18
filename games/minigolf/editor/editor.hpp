#pragma once
#include "document.hpp"
#include "course_preview.hpp"
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <map>
#include <mutex>
namespace GolfEditor {
struct Widget {
    SDL_FRect rect;
    std::string key, value;
    std::function<void()> action;
    std::function<void(const std::string&)> setter;
};
class Editor {
public:
    explicit Editor(FrameID frame);
    ~Editor();
    int run(const std::string& smokeOutput={});
    Document doc;
private:
    FrameID frame;
    SDL_Renderer* renderer;
    SDL_Window* window;
    TTF_Font* font=nullptr;
    SDL_Texture* scene=nullptr;
    CoursePreview preview;
    std::map<std::string,SDL_Texture*> textCache;
    std::vector<Widget> widgets;
    std::function<void(const std::string&)> focusSetter;
    std::string focus,buffer,status="Choose a tool, then click or drag on the course.";
    bool selectAll=true,running=true,testing=false,animated=false,dragging=false,dragChanged=false,panning=false,shooting=false,lockedSurfaces=false,grid=true;
    bool hideSurfaces=false,hideRails=false;
    bool addingWaypoints=false,choosingRail=false;
    int tool=0,pointIndex=0,dragPoint=-1;
    float zoom=1,inspectorScroll=0,inspectorBottom=88,mouseX=0,mouseY=0;
    Vec2 center{705,505},dragStart{},last{},panStart{},shotStart{};
    SDL_FRect canvas{};
    std::optional<Vec2> firstPortal;
    std::string paintGroup;
    float autosaveTimer=0;
    std::mutex dialogMutex;
    std::optional<std::pair<int,std::string>> dialogResult;
    bool dialogOpen=false;
    void paint();
    void event(const SDL_Event& e);
    void text(float x,float y,const std::string& value,SDL_Color color={225,233,240,255});
    void box(SDL_FRect r,SDL_Color c,bool fill=true);
    void button(SDL_FRect r,const std::string& label,std::function<void()> action,bool active=false,bool enabled=true);
    void field(float x,float y,float width,const std::string& key,const std::string& label,const std::string& value,std::function<void(const std::string&)> setter);
    void number(float y,const std::string& label,float value,std::function<void(float)> setter);
    void inspector();
    void commit();
    void resetPreview();
    void fit();
    void action(const std::function<void()>& f);
    void fileDialog(int kind);
    void save(bool as=false);
    bool confirmReplace(const std::filesystem::path& directory);
    bool discardAllowed();
    void recovery();
    void open(const std::filesystem::path& path);
    Vec2 world(float x,float y) const;
    Vec2 screen(Vec2 p) const;
    Vec2 displayedPosition(Object o) const;
    Vec2 snap(Vec2 p) const;
    std::optional<Object> hit(Vec2 p) const;
    std::vector<Object> objects() const;
    bool selectRail(float x,float y);
    void canvasDown(const SDL_MouseButtonEvent& e);
    void canvasMove(float x,float y);
    void canvasUp();
    void tile(Vec2 p);
    void updateTitle();
};
}
