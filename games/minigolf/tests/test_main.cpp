#include <exception>
#include <iostream>
#include <string>
void test_physics();
void test_obstacles();
void test_spawning();
void test_turns();
void test_settings();
void test_team_play();
void test_rendering(const std::string&);
int main(int argc,char** argv) {
    try {
        test_physics(); std::cout << "PASS minigolf/physics" << std::endl;
        test_obstacles(); std::cout << "PASS minigolf/obstacles" << std::endl;
        test_spawning(); std::cout << "PASS minigolf/spawning" << std::endl;
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
