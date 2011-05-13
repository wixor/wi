#!/usr/bin/env python
# -*- coding: utf-8 -*-

# Mateusz Dereniowski, Wiktor Janas
# Wyszukiwanie informacji lato 2011

import sys
import operator

def status(s):
	sys.stderr.write(s + "\r")

dummy_word = '"'

def count_words(filename):
	"""Scans through file of tokenized Wikipedia documents, counting distinct words."""
	words = {}
	words_count = 0
	distinct_words = 0
	dummy_words_count = 0
	### reading tokens ###
	with open(filename, 'rb', buffering=2**25) as f_TOKENS:
		for i, line in enumerate(f_TOKENS):
			status('Processing article {0}'.format(i))
			dummy_words_count -= 1 # title is separated from text with a dummy word
			for token in line.split():
				if token != dummy_word:
					words_count += 1
					if token in words:
						words[token] += 1
					else:
						words[token] = 1
						distinct_words += 1
				else:
					dummy_words_count += 1
		status('Processed {0} articles.\n'.format(i+1))

	sys.stderr.write("Our tokenizer distinguished {0} words with known characters and {1} with unknown. There were {2} distinct, correct words.".format(words_count, dummy_words_count, distinct_words))

	words_sorted = sorted(words.iteritems(), key=operator.itemgetter(1))
	for word, freq in words_sorted:
		print word + '\t' + str(freq)


def main():
	if len(sys.argv) < 2:
		print 'usage: ./words_frequency.py tokenized_wiki'
		exit(1)

	count_words(sys.argv[1])


if __name__ == "__main__":
	main()

