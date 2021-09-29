#!/bin/sh
set -eu
cd "$(dirname "$0")"
set -x
cc -Wall -Wextra -pedantic -std=c99 -fsanitize=address -Og -g \
    -I vendor -o json2tar json2tar.c vendor/jsont.c
