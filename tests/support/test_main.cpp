#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <QCoreApplication>

int main(int argc, char **argv) {
    auto const app = QCoreApplication{argc, argv};
    auto context = doctest::Context{argc, argv};
    return context.run();
}
