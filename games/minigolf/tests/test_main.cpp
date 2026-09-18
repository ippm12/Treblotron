#include <exception>
#include <iostream>
#include <string>
#include <cstdlib>
#include "debug/crash_reporting.hpp"
void test_physics();
void test_crushing();
void test_give_up();
void test_course_io();
void export_legacy_course(const std::string&);
void test_obstacles();
void test_spawning();
void test_turns();
void test_settings();
void test_team_play();
void test_rendering(const std::string&);
int main(int argc,char** argv) {
    int helperExit=0;
    if(runCrashDumpHelper(argc,argv,helperExit)) return helperExit;
#ifdef _WIN32
    if(!initializeCrashReporting()) {
        std::cerr << "Cannot initialize Mini Golf test crash reporting" << std::endl;
        return 1;
    }
#endif
    if(argc==2 && std::string(argv[1])=="--crash-probe") std::abort();
    try {
        if(argc==3 && std::string(argv[1])=="--export-course") { export_legacy_course(argv[2]); return 0; }
        test_course_io(); std::cout << "PASS minigolf/course_io" << std::endl;
        test_physics(); std::cout << "PASS minigolf/physics" << std::endl;
        test_crushing(); std::cout << "PASS minigolf/crushing" << std::endl;
        test_obstacles(); std::cout << "PASS minigolf/obstacles" << std::endl;
        test_spawning(); std::cout << "PASS minigolf/spawning" << std::endl;
        test_give_up(); std::cout << "PASS minigolf/give_up" << std::endl;
        test_turns(); std::cout << "PASS minigolf/turns" << std::endl;
        test_settings(); std::cout << "PASS minigolf/settings" << std::endl;
        test_team_play(); std::cout << "PASS minigolf/team_play" << std::endl;
        if(argc>1) { test_rendering(argv[1]); std::cout << "PASS minigolf/rendering" << std::endl; }
        return 0;
    } catch(const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
