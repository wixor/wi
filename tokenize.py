#!/usr/bin/env python
# -*- coding: utf-8 -*-

# Mateusz Dereniowski, Wiktor Janas
# Wyszukiwanie informacji lato 2011

import sys, os
from struct import pack
import re
import array
import marshal


dummy_word = '"'
marker = '"m'


def tokenize(wikipedia, interesting_arts):
	"""Tokenizing given text,\nTitles starts with tag: "##TITLE## "."""
	colon = re.compile(r': ')
	def unify_title(title):
		title = title.strip().replace('_', ' ')
		title = re.sub(colon, ':', title)
		return title

	sys.stderr.write("Loading list of interesting articles.\n")
	### reading titles of interesting articles, which are ought to be ranked ###
	ranked_articles = set()
	with open(interesting_arts, 'rb') as f_INTERESTING:
		for title in f_INTERESTING:
			title = unify_title(title.decode('utf8'))
			ranked_articles.add(title.lower())


	title_pattern = re.compile(r"^##TITLE## (.*)$")
	word = ur"""a-zß-öø-ÿĀ-ſ"""
	not_word = ur"""[^{0}]""".format(word) # ąęśćżźńółèüäéáúūōíñõ
	unwanted_char = re.compile(ur"[^-{word}0-9.']".format(word=word), re.U)
	symbol = ur"""[,"!?:;@#$%^&*()+_=[\]{}\|<>/]"""
	unwanted_symbol = re.compile(ur"(?:{symbol})|(?:(?<={not_word})[-.'])|(?:[-.'](?={not_word}))|(?:^[-.'])|(?:[-.']$)".format(symbol=symbol, not_word=not_word), re.U)
	parenthesis = re.compile(ur"[()]", re.U)

	titles_count = 0
	terms_per_article = 0
	articles_titles = []
	articles_terms_count = array.array('H')
	### reading and parsing file ###
	sys.stderr.write("Tokenizing articles.\n")
	with open(wikipedia, 'rb', buffering=2**27) as f_SRC:
	 with open('db/tokenized', 'wb', buffering=2**25) as f_TOKENS:
		for i, line in enumerate(f_SRC):
			# percentage progress
			if i % 1000 == 0:
				sys.stderr.write("%.2f" % (100.0 * i / 15255080) + "%   \r")
			# /percentage progress

			line = line.decode('utf8')
			title_match = title_pattern.match(line)
			if title_match:
				if titles_count != 0:
					f_TOKENS.write('\n')
					articles_terms_count.append(terms_per_article)
				titles_count += 1
				title = unify_title(title_match.group(1))
				articles_titles.append(title)
				dummy_or_marked = None # title separator, also works as interesting articles marker
				lower_title = title.lower()
				if lower_title in ranked_articles:
					dummy_or_marked = marker
					ranked_articles.remove(lower_title)
				else:
					dummy_or_marked = dummy_word
				title_wo_parent = re.sub(parenthesis, ur"", lower_title)
				terms_per_article = len(title.split()) + 1 # +1 for title separator
				f_TOKENS.write(' '.join((title_wo_parent.encode('utf8'), dummy_or_marked)))
			else:
				line = line.lower()
				line = re.sub(unwanted_symbol, ur" ", line)
				tokens = line.split()
				terms_per_article += len(tokens)
				for token in tokens:
					if unwanted_char.search(token):
						f_TOKENS.write(' ' + dummy_word)
					else:
						f_TOKENS.write(' ' + token.encode('utf8'))
		f_TOKENS.write('\n')
		articles_terms_count.append(terms_per_article)

		if ranked_articles:
			print "Interesting articles that weren't in wikipedia:"
			print '\n'.join(ranked_articles).encode('utf8')
			print
	
	sys.stderr.write("Writing to 'db/artitles'.\n")
	with open('db/artitles', 'wb', buffering=2**25) as f_TITL:
		f_TITL.write('TITL')
		f_TITL.write(pack('<I', titles_count))
		f_TITL.write(bytearray(titles_count * 4))

		for terms_count in articles_terms_count:
			f_TITL.write(pack('<H', terms_count))

		for title in articles_titles:
			f_TITL.write(title.encode('utf8') + "\x00")
	
	with open('db/articles.marshal', 'wb') as f_ARTICLES:
		marshal.dump(articles_titles, f_ARTICLES)


def main():
	if len(sys.argv) != 3:
		print 'usage: ./tokenize.py wiki_src interesting_arts > interesting_not_in_wikipedia.txt'
		exit(1)

	tokenize(sys.argv[1], sys.argv[2])


if __name__ == "__main__":
	main()

