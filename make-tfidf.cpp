#include <assert.h>
#include "bufrw.h"
#include "fileio.h"
#include "corpus.h"
#include "rawpost.h"

class TfIdfMaker
{
    Writer tfs, idfs;
    int last_term_id, last_doc_id, tf, idf;

    inline TfIdfMaker() { }

    inline void flush_doc(int next_doc_id)
    {
        if(last_doc_id != -1) 
            tfs.write_u64(rawpost(last_term_id, last_doc_id, tf));
        last_doc_id = next_doc_id;
        tf = 1;
    }

    inline void flush_term(int next_term_id)
    {
        for(int i=last_term_id+1; i<next_term_id; i++)
            idfs.write_u32(0);
        if(last_term_id != -1)
            idfs.write_u32(idf);
        last_term_id = next_term_id;
        idf = 1;
    }

    void run(Reader rd, int n_terms)
    {
        last_term_id = last_doc_id = -1;
        tf = idf = 0;

        idfs.write_u32(0x52464449);
        idfs.write_u32(n_terms);

        while(!rd.eof())
        {
            rawpost e(rd.read_u64());

            if(e.term_id() == last_term_id) 
                if(e.doc_id() == last_doc_id) 
                    tf++;
                else {
                    idf++;
                    flush_doc(e.doc_id());
                }
            else {
                flush_doc(e.doc_id());
                flush_term(e.term_id());
            }
        }

        flush_doc(0);
        flush_term(n_terms);
    }

public:
    static void run(Reader rd, FileIO tffile, FileIO idffile, int n_terms)
    {
        TfIdfMaker mk;
        mk.run(rd, n_terms);

        tffile.write_raw(mk.tfs.buffer(), mk.tfs.tell());
        idffile.write_raw(mk.idfs.buffer(), mk.idfs.tell());
    }
};

int main(void)
{
    Corpus corp("db/corpus");

    FileMapping invlemmap("db/invlemma");
    Reader rd(invlemmap.data(), invlemmap.size());

    FileIO tffile("db/tfs", O_WRONLY|O_CREAT|O_TRUNC, 0666),
           idffile("db/idfs", O_WRONLY|O_CREAT|O_TRUNC, 0666);

    TfIdfMaker::run(rd, tffile, idffile, corp.size());

    return 0;
}

