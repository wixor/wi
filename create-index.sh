#!/bin/sh
WIKI=$1
MORFO=$2
set -e
rm -f db/*

BOLD="\033[1m"
NORM="\033[22m"
step() { echo "$BOLD --- $1$NORM"; }

step "building corpus"
./make-corpus $WIKI $MORFO
step "parsing morphologic"
./make-binmorfo $MORFO
step "finding aliases"
./make-aliases
step "building mosquare"
./make-mosquare
step "digitizing wiki"
./digitize < $WIKI > db/digital
step "lemmatizing wiki"
./lemmatize < db/digital > db/dilemma
step "inverting wiki"
./invert db/digital && mv db/digital db/inverted
step "inverting lemmatized wiki"
./invert db/dilemma && mv db/dilemma db/invlemma
step "building index"
./make-index

