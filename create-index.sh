#!/bin/sh
WIKI=$1
MORFO=$2
set -e
rm -f db/*

BOLD="\033[1m"
NORM="\033[22m"
step() { echo "$BOLD --- $1$NORM"; }

step "tokenizing wiki"
./tokenize.py $WIKI
step "building corpus"
./make-corpus db/tokenized $MORFO
step "parsing morphologic"
./make-binmorfo $MORFO
step "finding aliases"
./make-aliases
step "building mosquare"
./make-mosquare
step "digitizing wiki"
./digitize < db/tokenized > db/digital
step "inverting wiki"
./invert db/digital > db/inverted && rm db/digital
step "lemmatizing wiki"
./lemmatize < db/inverted > db/dilemma
step "inverting lemmatized wiki"
./invert db/dilemma > db/invlemma && rm db/dilemma
step "building index"
./make-index

