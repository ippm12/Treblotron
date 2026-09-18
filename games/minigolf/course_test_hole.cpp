#include "course_io.hpp"
#include <stdexcept>
namespace MiniGolf {
Course buildCourse(CourseId id) {
    if(id!=CourseId::TestHole) throw std::runtime_error("Unknown built-in course");
    return loadCourse(defaultCourseDirectory());
}
}
