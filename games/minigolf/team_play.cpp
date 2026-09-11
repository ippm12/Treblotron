#include "minigolf.hpp"
#include "players/players.hpp"
#include "vision/vision.hpp"
#include "game_lib/components/render_shape.hpp"
#include "game_lib/components/render_text.hpp"
#include "frame/render_queue.hpp"
#include <algorithm>
#include <cmath>

namespace MiniGolf {
void MiniGolfGame::configureTeams(uint8_t count)
{
    m_teams.clear();
    if(m_options.teams==TeamMode::Individual) return;
    std::vector<TeamID> ids;
    for(uint8_t i=0;i<count;++i) {
        const auto id=getPlayerTeam(getPlayerByIndex(i));
        auto found=std::find(ids.begin(),ids.end(),id);
        // Unassigned players each remain an individual team.
        if(id==INVALID_TEAM_ID || found==ids.end()) {
            ids.push_back(id);
            TeamState team;
            team.name=id==INVALID_TEAM_ID ? getPlayerName(getPlayerByIndex(i)) : getTeamName(id);
            team.members.push_back(i);
            m_teams.push_back(team);
        } else m_teams[static_cast<size_t>(found-ids.begin())].members.push_back(i);
    }
}
uint8_t MiniGolfGame::activeMember() const
{
    if(m_teams.empty() || m_currentPlayer>=m_teams.size()) return m_currentPlayer;
    const auto& team=m_teams[m_currentPlayer];
    return team.members[team.cursor%team.members.size()];
}
std::string MiniGolfGame::competitorName(size_t i) const
{
    if(!m_teams.empty() && i<m_teams.size()) return m_teams[i].name;
    const auto id=getPlayerByIndex(static_cast<uint8_t>(i));
    return id==INVALID_PLAYER_ID ? "Player "+std::to_string(i+1) : getPlayerName(id);
}
void MiniGolfGame::startTeamAttempt()
{
    if(m_teams.empty()) return;
    auto& team=m_teams[m_currentPlayer];
    auto& p=m_players[m_currentPlayer];
    p.ballColor=BALL_COLORS[activeMember()%BALL_COLORS.size()];
    if(m_options.teams!=TeamMode::Scramble || team.attemptStarted) return;
    if(team.attempts.empty()) {
        team.origin=p.shotStart;
        if(b2Body_IsValid(p.ballBody)) getBodyPositionPx(*m_world,p.ballBody,team.origin.x,team.origin.y);
        team.baseStrokes=p.strokes[m_currentHole];
    }
    p.strokes[m_currentHole]=team.baseStrokes;
    p.finishedHole[m_currentHole]=false; p.holedOut[m_currentHole]=false;
    p.shotStart=team.origin;
    p.hazardImmunity=0; p.respawnProtected=false;
    spawnBall(m_currentPlayer,team.origin);
    team.attemptStarted=true;
}
bool MiniGolfGame::finishTeamAttempt()
{
    if(m_teams.empty()) return false;
    auto& team=m_teams[m_currentPlayer];
    if(m_options.teams==TeamMode::AlternateShot) {
        team.cursor=(team.cursor+1)%team.members.size();
        return false;
    }
    if(m_options.teams!=TeamMode::Scramble) return false;
    auto& p=m_players[m_currentPlayer];
    Vec2 position=p.shotStart;
    if(p.holedOut[m_currentHole]) position=cupPosition();
    else if(b2Body_IsValid(p.ballBody)) getBodyPositionPx(*m_world,p.ballBody,position.x,position.y);
    team.attempts.push_back({position,p.strokes[m_currentHole],p.holedOut[m_currentHole],activeMember()});
    if(b2Body_IsValid(p.ballBody)) b2DestroyBody(p.ballBody);
    p.ballBody=b2_nullBodyId;
    p.pendingReturn=false; p.trail.clear();
    p.strokes[m_currentHole]=team.baseStrokes;
    p.finishedHole[m_currentHole]=false; p.holedOut[m_currentHole]=false;
    team.attemptStarted=false;
    ++team.attemptsTaken;
    m_throwsRemainingInTurn=0;
    m_waitingForCollect=!isBoardClear();
    if(team.attemptsTaken>=team.members.size()) {
        m_phase=Phase::ScrambleChoice;
        m_scrambleChoice=0;
    } else {
        team.cursor=(team.cursor+1)%team.members.size();
        m_phase=Phase::Aiming;
        if(!m_waitingForCollect) beginNextTurn();
    }
    return true;
}
void MiniGolfGame::chooseScramble(size_t choice)
{
    if(m_phase!=Phase::ScrambleChoice || m_teams.empty()) return;
    // Physical collection must finish before accepting the next set of darts.
    auto& team=m_teams[m_currentPlayer];
    if(choice>=team.attempts.size()) return;
    const auto selected=team.attempts[choice];
    auto& p=m_players[m_currentPlayer];
    p.strokes[m_currentHole]=selected.strokes;
    p.shotStart=selected.position;
    if(selected.holed) {
        p.holedOut[m_currentHole]=true; p.finishedHole[m_currentHole]=true;
    } else {
        spawnBall(m_currentPlayer,selected.position);
        p.finishedHole[m_currentHole]=selected.strokes>=STROKE_CAP;
    }
    team.attempts.clear(); team.attemptsTaken=0; team.attemptStarted=false;
    team.cursor=(team.cursor+1)%team.members.size();
    m_committingScramble=true;
    endCurrentTurn();
    m_committingScramble=false;
}
void MiniGolfGame::onMouseClick(float x,float y,uint8_t button)
{
    if(button!=1 || m_phase!=Phase::ScrambleChoice || x<COURSE_VIEW_W+20 || x>1900) return;
    const int choice=static_cast<int>((y-180)/72);
    if(y>=180 && choice>=0 && static_cast<size_t>(choice)<m_teams[m_currentPlayer].attempts.size())
        chooseScramble(static_cast<size_t>(choice));
}
void MiniGolfGame::renderScramble()
{
    if(m_options.teams!=TeamMode::Scramble || m_teams.empty()) return;
    const auto fid=getFrameId();
    auto text=[&](std::string value,float x,float y,Color color,uint32_t z,float scale=1.0f) {
        auto t=std::make_shared<RenderText>();
        t->m_text=value; t->m_fontId=m_fontId; t->m_x=x; t->m_y=y;
        t->m_color=color; t->m_z=z; t->m_rotation=0; t->m_scaleX=scale; t->m_scaleY=scale;
        renderQueueAdd(fid,t);
    };
    const auto& team=m_teams[m_currentPlayer];
    for(size_t i=0;i<team.attempts.size();++i) {
        const auto& a=team.attempts[i];
        float x,y; m_camera.worldToScreen(a.position.x,a.position.y,x,y);
        auto ring=std::make_shared<RenderShape>();
        ring->m_type=ShapeType::Circle; ring->m_x=x; ring->m_y=y;
        ring->m_width=2*BALL_RADIUS_PX; ring->m_height=0; ring->m_rotation=0;
        ring->m_color=BALL_COLORS[a.member%BALL_COLORS.size()]; ring->m_z=CourseLayer::Ball+2;
        renderQueueAdd(fid,ring);
        text(std::to_string(i+1),x-8,y-16,{255,255,255},CourseLayer::Ball+3);
    }
    if(m_phase!=Phase::ScrambleChoice) return;
    auto panel=std::make_shared<RenderShape>();
    panel->m_type=ShapeType::Box; panel->m_x=COURSE_VIEW_W; panel->m_y=0;
    panel->m_width=1920-COURSE_VIEW_W; panel->m_height=GameLayout::BAR_Y;
    panel->m_rotation=0; panel->m_z=150; panel->m_color={25,30,40};
    renderQueueAdd(fid,panel);
    text("Choose a scramble result",COURSE_VIEW_W+25,40,{245,245,245},151);
    text(team.name,COURSE_VIEW_W+25,90,{180,205,240},151);
    for(size_t i=0;i<team.attempts.size();++i) {
        const auto& a=team.attempts[i];
        std::string name=getPlayerName(getPlayerByIndex(a.member));
        if(name.empty()) name="Player "+std::to_string(a.member+1);
        if(name.size()>18) name=name.substr(0,15)+"...";
        const std::string row=(i==m_scrambleChoice ? "> " : "  ")+std::to_string(i+1)+". "+name;
        text(row,COURSE_VIEW_W+25,180+72*i,BALL_COLORS[a.member%BALL_COLORS.size()],151);
        text(std::to_string(a.strokes-team.baseStrokes)+" strokes"+(a.holed ? " - Holed!" : ""),
             COURSE_VIEW_W+65,212+72*i,{220,220,220},151,0.75f);
    }
    text("Up/Down + Enter, or click a result",COURSE_VIEW_W+25,680,{190,200,210},151,0.75f);
    text("Numbered markers show saved results.",COURSE_VIEW_W+25,720,{190,200,210},151,0.7f);
}
}
