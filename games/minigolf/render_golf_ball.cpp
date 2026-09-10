#include "render_golf_ball.hpp"
#include <SDL3/SDL.h>
#include <array>
#include <algorithm>
#include <cmath>

namespace MiniGolf {
Status RenderGolfBall::render(FrameID frameId) const
{
    SDL_Renderer* renderer = getFrameRenderer(frameId);
    if(!renderer) return STATUS_ERROR_INVALID_PARAM;
    // Each clipped dimple triangle can produce at most two triangles.
    constexpr int DIMPLE_COUNT = 96;
    constexpr float DIMPLE_RADIUS = 0.095f;
    std::array<SDL_Vertex, 32*3 + DIMPLE_COUNT*8*6> vertices{};
    int count = 0;
    auto vertex = [&](float x, float y, float shade) {
        SDL_Vertex v{};
        v.position = {m_x+x*radius, m_y+y*radius};
        v.color = {std::clamp(color.r/255.0f*shade, 0.0f, 1.0f),
                   std::clamp(color.g/255.0f*shade, 0.0f, 1.0f),
                   std::clamp(color.b/255.0f*shade, 0.0f, 1.0f), 1};
        return v;
    };
    static const auto ring = [] {
        std::array<RollVector, 32> points{};
        for(int i=0;i<32;++i) {
            const float a=static_cast<float>(i)*6.283185307f/32;
            points[i]={std::cos(a),std::sin(a),0};
        }
        return points;
    }();
    for(int i=0;i<32;++i) {
        vertices[count++]=vertex(0,0,1.0f);
        vertices[count++]=vertex(ring[i].x,ring[i].y,1.0f);
        vertices[count++]=vertex(ring[(i+1)%32].x,ring[(i+1)%32].y,1.0f);
    }
    // Cache spherical patches once. Rotate/project each patch, clipping its
    // triangles against the front hemisphere to avoid popping at the rim.
    static const auto patches = [] {
        std::array<std::array<RollVector,9>,DIMPLE_COUNT> result{};
        for(int i=0;i<DIMPLE_COUNT;++i) {
            const float z=1-2*(static_cast<float>(i)+0.5f)/static_cast<float>(DIMPLE_COUNT);
            const float r=std::sqrt(1-z*z), a=static_cast<float>(i)*2.39996323f;
            const RollVector n{r*std::cos(a),r*std::sin(a),z};
            const RollVector u{-std::sin(a),std::cos(a),0};
            const RollVector v{-z*std::cos(a),-z*std::sin(a),r};
            result[i][0]=n;
            for(int j=0;j<8;++j) {
                const float t=static_cast<float>(j)*6.283185307f/8;
                const float c=std::cos(t)*std::sin(DIMPLE_RADIUS), s=std::sin(t)*std::sin(DIMPLE_RADIUS);
                result[i][j+1]={n.x*std::cos(DIMPLE_RADIUS)+u.x*c+v.x*s,
                    n.y*std::cos(DIMPLE_RADIUS)+u.y*c+v.y*s,n.z*std::cos(DIMPLE_RADIUS)+u.z*c+v.z*s};
            }
        }
        return result;
    }();
    for(const auto& patch : patches) {
        std::array<RollVector,9> p{};
        for(int j=0;j<9;++j) p[j]=orientation.rotate(patch[j]);
        for(int j=0;j<8;++j) {
            const std::array<RollVector,3> tri{p[0],p[j+1],p[(j+1)%8+1]};
            std::array<RollVector,4> clipped{};
            int n=0;
            for(int k=0;k<3;++k) {
                const auto a=tri[k], b=tri[(k+1)%3];
                if(a.z>=0) clipped[n++]=a;
                if((a.z>=0)!=(b.z>=0)) {
                    const float t=a.z/(a.z-b.z);
                    clipped[n++]={a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,0};
                }
            }
            for(int k=1;k+1<n;++k) {
                for(int index : {0,k,k+1}) {
                    const auto v=clipped[index];
                    // Flat, subtle markings match the course palette. Their
                    // projection conveys rolling without directional lighting.
                    vertices[count++]=vertex(v.x,v.y,0.78f);
                }
            }
        }
    }
    return SDL_RenderGeometry(renderer,nullptr,vertices.data(),count,nullptr,0)
        ? STATUS_OK : STATUS_ERROR_LIB_CALL;
}
}
