#include "course_themes.hpp"
#include "fixtures/golf_fixture.hpp"

void test_rendering(const std::string& output)
{
    GolfFixture fixture;
    [[maybe_unused]] auto& g=fixture.game;
    [[maybe_unused]] auto& course=fixture.course;
    [[maybe_unused]] auto& base=fixture.base;
    assert(initializeFrameModule()==STATUS_OK);
    FrameID frame;
    assert(createNewFrame("Obstacle render checks",1920,1080,frame)==STATUS_OK);
    g.m_frameId=frame;
    // Queue in reverse layer order: the higher red box must remain visible.
    for(auto z:{CourseLayer::Guide,CourseLayer::Cup,CourseLayer::Rail,CourseLayer::Ice}) {
        auto box=std::make_shared<RenderShape>();
        box->m_type=ShapeType::Box; box->m_x=0; box->m_y=0;
        box->m_width=20; box->m_height=20; box->m_rotation=0; box->m_z=z;
        box->m_color=z==CourseLayer::Guide ? Color{255,0,0}:Color{0,255,0};
        renderQueueAdd(frame,box);
    }
    assert(renderQueueSortByLayer(frame)==STATUS_OK);
    assert(renderQueueDrawFlush(frame)==STATUS_OK);
    auto* layerImage=SDL_RenderReadPixels(getFrameRenderer(frame),nullptr);
    assert(layerImage);
    Uint8 red=0,green=0,blue=0,alpha=0;
    assert(SDL_ReadSurfacePixel(layerImage,10,10,&red,&green,&blue,&alpha));
    assert(red==255 && green==0 && blue==0);
    SDL_DestroySurface(layerImage);
    g.m_fontId=loadFont("assets/fonts/Roboto-Regular.ttf",28);
    g.m_compassFontId=loadFont("assets/fonts/Roboto-Regular.ttf",42);
    prepare(g,base); place(g,{705,300}); g.m_phase=Phase::Aiming;
    renderQueueClearFrame(frame,20,25,30);
    g.renderHashCompass();
    assert(renderQueueSortByLayer(frame)==STATUS_OK);
    assert(renderQueueDrawFlush(frame)==STATUS_OK);
    auto* headerImage=SDL_RenderReadPixels(getFrameRenderer(frame),nullptr);
    assert(headerImage);
    for(int y=0;y<static_cast<int>(COURSE_VIEW_Y);++y)
        for(int x=0;x<static_cast<int>(COURSE_VIEW_W);++x) {
            assert(SDL_ReadSurfacePixel(headerImage,x,y,&red,&green,&blue,&alpha));
            assert(red==20 && green==25 && blue==30);
        }
    SDL_DestroySurface(headerImage);
    for(size_t i=0;i<4;++i) {
        prepare(g,course.holes[i]);
        g.m_course.holes[i]=course.holes[i];
        g.m_currentHole=static_cast<uint8_t>(i);
        g.m_courseTime=0.7;
        if(i==1) {
            g.m_bumperAnimation[0]=0.25f;
            g.m_players[0].destructionTimer=0.25f;
            g.m_players[0].destructionPosition={705,500};
        }
        for(size_t j=0;j<g.m_players.size();++j) g.m_players[j].ballColor=BALL_COLORS[j];
        renderQueueClearFrame(frame,20,25,30);
        g.renderCourse(); g.renderObstacles(); g.renderBalls();
        if(i==2) { g.m_phase=Phase::Aiming; g.renderHashCompass(); }
        assert(renderQueueSortByLayer(frame)==STATUS_OK);
        assert(renderQueueDrawFlush(frame)==STATUS_OK);
        auto* surface=SDL_RenderReadPixels(getFrameRenderer(frame),nullptr);
        assert(surface);
        const std::string path=output+"/hole-"+std::to_string(i+1)+".bmp";
        assert(SDL_SaveBMP(surface,path.c_str()));
        SDL_DestroySurface(surface);
    }
    prepare(g,base);
    g.m_options.teams=TeamMode::Scramble;
    TeamState sample; sample.name="Team A"; sample.members={0,1,2};
    sample.attempts={{{500,500},3,false,0},{{800,400},3,false,1},{{1000,600},4,false,2}};
    g.m_teams={sample}; g.m_phase=Phase::ScrambleChoice;
    renderQueueClearFrame(frame,20,25,30);
    g.renderCourse(); g.renderObstacles(); g.renderScramble();
    assert(renderQueueSortByLayer(frame)==STATUS_OK && renderQueueDrawFlush(frame)==STATUS_OK);
    auto* selectionImage=SDL_RenderReadPixels(getFrameRenderer(frame),nullptr);
    assert(selectionImage);
    assert(SDL_SaveBMP(selectionImage,(output+"/scramble.bmp").c_str()));
    SDL_DestroySurface(selectionImage);
    {
        MainMenu menu; menu.m_frameId=frame;
        menu.m_titleFontId=g.m_compassFontId; menu.m_cardFontId=g.m_fontId; menu.m_smallFontId=g.m_fontId;
        const auto& games=getRegisteredGames();
        menu.m_selectedGameIndex=static_cast<size_t>(std::find_if(games.begin(),games.end(),
            [](const auto& d){return d.name=="Mini Golf";})-games.begin());
        menu.m_settingChoices={0,1,1,2,2}; menu.m_settingsCursor=3;
        renderQueueClearFrame(frame,20,25,30); menu.renderGameSettings();
        assert(renderQueueDrawFlush(frame)==STATUS_OK);
        auto* settingsImage=SDL_RenderReadPixels(getFrameRenderer(frame),nullptr);
        assert(settingsImage);
        assert(SDL_SaveBMP(settingsImage,(output+"/settings.bmp").c_str()));
        SDL_DestroySurface(settingsImage);
    }
    prepare(g,base);
    g.m_camera.setZoom(0.75f);
    for(const auto& theme:COURSE_THEMES) {
        g.m_course.theme=theme.id;
        renderQueueClearFrame(frame,20,25,30);
        g.renderCourse(); g.renderObstacles(); g.renderBalls();
        assert(renderQueueSortByLayer(frame)==STATUS_OK && renderQueueDrawFlush(frame)==STATUS_OK);
        auto* themed=SDL_RenderReadPixels(getFrameRenderer(frame),nullptr);
        assert(themed);
        assert(SDL_SaveBMP(themed,(output+"/theme-"+theme.id+".bmp").c_str()));
        assert(SDL_ReadSurfacePixel(themed,500,30,&red,&green,&blue,&alpha));
        assert(red==20 && green==25 && blue==30);
        SDL_DestroySurface(themed);
    }
    // Exercise all 63 hole initializations, moving objects and shared rendering.
    // Each atlas contains holes 1-9 in reading order for author review.
    for(const auto& theme:COURSE_THEMES) {
        if(std::string(theme.id)=="classic") continue;
        const auto authored=loadCourse(defaultCourseDirectory().parent_path()/theme.id);
        auto* atlas=SDL_CreateSurface(1410,849,SDL_PIXELFORMAT_RGBA32);
        assert(atlas);
        for(size_t n=0;n<authored.holes.size();++n) {
            g.m_course.theme=authored.theme;
            prepare(g,authored.holes[n]);
            g.m_camera.setZoom(1.0f);
            for(size_t player=0;player<g.m_players.size();++player) g.m_players[player].ballColor=BALL_COLORS[player];
            for(int tick=0;tick<240;++tick) g.stepCoursePhysics(1.0f/120);
            renderQueueClearFrame(frame,20,25,30);
            g.renderCourse(); g.renderObstacles(); g.renderBalls();
            assert(renderQueueSortByLayer(frame)==STATUS_OK && renderQueueDrawFlush(frame)==STATUS_OK);
            auto* shot=SDL_RenderReadPixels(getFrameRenderer(frame),nullptr);
            assert(shot);
            SDL_Rect source{0,80,1410,850},destination{int(n%3)*470,int(n/3)*283,470,283};
            assert(SDL_BlitSurfaceScaled(shot,&source,atlas,&destination,SDL_SCALEMODE_LINEAR));
            SDL_DestroySurface(shot);
        }
        assert(SDL_SaveBMP(atlas,(output+"/course-"+theme.id+".bmp").c_str()));
        SDL_DestroySurface(atlas);
    }
    g.shutdown(); deleteFrame(frame); shutdownFrameModule();
}
