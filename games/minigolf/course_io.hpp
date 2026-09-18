#pragma once
#include "course_defs.hpp"
#include <filesystem>

namespace MiniGolf {
// All failures throw std::runtime_error with file/field context. Loading never
// changes an existing course; save validates the entire course before writing.
Course loadCourse(const std::filesystem::path& directory);
void saveCourse(const Course& course, const std::filesystem::path& directory);
void validateCourse(const Course& course);
// Assign missing IDs once when creating/importing objects, then retain them.
void assignCourseIds(Course& course);
std::filesystem::path defaultCourseDirectory();
struct CourseFile {
    std::string name;
    std::filesystem::path directory;
};
// Bundled courses plus writable local copies; matching directory names override.
std::vector<CourseFile> availableCourses();
}
