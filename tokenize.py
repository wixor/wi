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

def tokenize(wikipedia, interesting_arts, wikilinks):
	"""Tokenizing given text,\nTitles starts with tag: "##TITLE## "."""

	sys.stderr.write("Loading list of interesting articles.\n")
	### reading titles of interesting articles, which are ought to be ranked ###
	ranked_articles = set()
	colon = re.compile(r': ')
	with open(interesting_arts, 'rb') as f_INTERESTING:
		for title in f_INTERESTING:
			title = title.decode('utf8')
			title = title.strip()
			title = title.replace('_', ' ')
			title = re.sub(colon, ':', title)
			ranked_articles.add(title)

	### reading all articles titles from 'wikilinki.txt' ###
	if os.path.exists('db/wikilinki.marshal'):
		with open('db/wikilinki.marshal', 'rb') as f:
			sys.stderr.write("Loading previously parsed 'wikilinki.txt'...\n")
			articles_from_links = marshal.load(f)
			sys.stderr.write("...finished.\n")
	else:
		sys.stderr.write("Loading articles' titles from 'wikilinki.txt'...\n")
		articles_from_links = set()
		with open(wikilinks, 'rb', buffering=2**27) as f_WIKILINKS:
			for title in f_WIKILINKS:
				title = title.decode('utf8')
				title = title.strip()
				title = title.lower()
				title = title.replace('_', ' ')
				title = re.sub(colon, ':', title)
				articles_from_links.add(title)
		with open('db/wikilinki.marshal', 'wb') as f:
			marshal.dump(articles_from_links, f)
		sys.stderr.write("...finished.\n")

	title_colon = re.compile(r"^##TITLE## (.*)$")
	word = ur"""a-zß-öø-ÿĀ-ſ"""
	not_word = ur"""[^{0}]""".format(word) # ąęśćżźńółèüäéáúūōíñõ
	unwanted_char = re.compile(ur"[^-{word}0-9.']".format(word=word), re.U)
	symbol = ur"""[,"!?:;@#$%^&*()+_=[\]{}\|<>/]"""
	unwanted_symbol = re.compile(ur"(?:{symbol})|(?:(?<={not_word})[-.'])|(?:[-.'](?={not_word}))|(?:^[-.'])|(?:[-.']$)".format(symbol=symbol, not_word=not_word), re.U)
	parenthesis = re.compile(ur"[()]", re.U)

	tytuly_count = 0
	terms_per_article = 1 # title separator counts as word too
	articles_titles = []
	articles_terms_count = array.array('H')
	### reading and parsing file ###
	sys.stderr.write("Tokenizing articles.\n")
	with open(wikipedia, 'rb', buffering=2**27) as f_SRC:
	 with open('db/tokenized', 'wb', buffering=2**25) as f_TOKENS:
		for i, line in enumerate(f_SRC):
			if i % 10000 == 0:
				sys.stderr.write("%.2f" % (100.0 * i / 15255080) + "%   \r")
			line = line.decode('utf8')
			title = title_colon.match(line)
			if title:
				if tytuly_count != 0:
					f_TOKENS.write('\n')
					articles_terms_count.append(terms_per_article)
				tytuly_count += 1
				tytul = title.group(1).strip()
				tytul = re.sub(colon, ':', tytul)
				articles_titles.append(tytul)
				dummy_or_marked = None
				if tytul in ranked_articles:
					dummy_or_marked = marker
					ranked_articles.remove(tytul)
				else:
					dummy_or_marked = dummy_word
				tytul = tytul.lower()
				tytul = re.sub(parenthesis, ur"", tytul)
				terms_per_article = len(tytul.split()) + 1
				f_TOKENS.write(' '.join((tytul.encode('utf8'), dummy_or_marked)))
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
			sys.stderr.write("\nranked articles that left unset:\n" + '\n'.join(ranked_articles) + '\n')

		print "articles that weren't in 'wikilinki.txt':"
		new_set = set()
		for title in articles_titles:
			lower_title = title.lower()
			if lower_title in articles_from_links:
				articles_from_links.remove(lower_title)
			elif title in new_set: # repeated article title
				sys.stderr.write('repeated\t' + title.encode('utf8') + '\n')
			else:
				print title.encode('utf8')
			new_set.add(title)

		for _ in articles_from_links:
			f_TOKENS.write(dummy_word + '\n')


	with open('db/artitles', 'wb', buffering=2**25) as f_TITL:
		tytuly_count_total = tytuly_count + len(articles_from_links)
		f_TITL.write('TITL')
		f_TITL.write(pack('<I', tytuly_count_total))
		f_TITL.write(bytearray(tytuly_count_total * 4 * 2))

		for terms_count in articles_terms_count:
			f_TITL.write(pack('<H', terms_count))
		for _ in articles_from_links:
			f_TITL.write(pack('<H', 1))

		for tytul in articles_titles:
			f_TITL.write(tytul.encode('utf8') + "\x00")
		for tytul in articles_from_links:
			f_TITL.write(tytul.encode('utf8') + "\x00")



def main():
	if len(sys.argv) < 2:
		print 'usage: ./tokenize.py wiki_src interesting_arts wikilinks'
		exit(1)

	tokenize(sys.argv[1], sys.argv[2], sys.argv[3])


if __name__ == "__main__":
	main()

