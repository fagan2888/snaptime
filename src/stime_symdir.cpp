/* for now these all get converted to floats. this should change eventually */
#include <limits.h>
#include <stdlib.h>
void TSTimeSymDir::InflateData(TTimeCollection &r, TStr initialTs, double duration,
                               double granularity, std::vector<std::vector<double>> &result)
{
    TTime initialTimestamp = Schema.ConvertTime(initialTs);
    int size = duration / granularity;
    // initialize as a grid of 0s
    for (int i = 0; i < r.Len(); i++)
    {
        std::vector<double> empty_row(size);
        result.push_back(empty_row);
    }

    for (int i = 0; i < r.Len(); i++)
    { // for each result
        TPt<TSTime> &data_ptr = r.TimeCollection[i];
        int data_length = data_ptr->Len();
        if (data_length == 0)
        {
            // deal with empty result
            continue;
        }
        int index = data_ptr->GetFirstValueWithTime(initialTimestamp); // find initial index
        double val = data_ptr->GetFloat(index);						   // first value
        for (int j = 0; j < size; j++)
        { // for each time stamp
            TTime ts = initialTimestamp + j * granularity;
            int new_index;
            if (index >= data_length - 1)
            {
                new_index = index; // at the end of the vector
            }
            else
            {
                new_index = AdvanceIndex(data_ptr, ts, index); // find the next index
            }
            if (new_index != index)
            { // this is a new value
                index = new_index;
                val = data_ptr->GetFloat(index);
            }
            result[i][j] = val;
        }
    }
}

int TSTimeSymDir::AdvanceIndex(TPt<TSTime> data_ptr, TTime time_stamp, int curr_index)
{
    if (curr_index >= data_ptr->Len() - 1)
    {
        // at end of vector so keep index the same
        return curr_index;
    }
    int index_after;
    for (index_after = curr_index + 1; index_after < data_ptr->Len(); index_after++)
    {
        TTime next_ts = data_ptr->DirectAccessTime(index_after);
        if (next_ts > time_stamp)
        {
            // we hit the end of the window, so break out
            return index_after - 1;
        }
    }
    return index_after - 1;
}

void TSTimeSymDir::SaveQuerySet(TTimeCollection &r, TSOut &SOut)
{
    SOut.Save(r.Len());
    for (int i = 0; i < r.Len(); i++)
    {
        r.TimeCollection[i]->Save(SOut);
    }
}

void TSTimeSymDir::LoadQuerySet(TTimeCollection &r, TSIn &SIn)
{
    int length = 0;
    SIn.Load(length);
    for (int i = 0; i < length; i++)
    {
        TPt<TSTime> queryRow = TSTime::LoadSTime(SIn);
        r.Add(queryRow);
    }
}

// Returns a query result. If OutputFile is not "", save into OutputFile
void TSTimeSymDir::QueryFileSys(TVec<FileQuery> &Query, TTimeCollection &r,
                                TStr &InitialTimeStamp, TStr &FinalTimeStamp,
                                TStr OutputFile, bool zeroFlag)
{
    // First find places where we can index by the symbolic filesystem
    THash<TStr, FileQuery> QueryMap;
    GetQuerySet(Query, QueryMap);
    TVec<FileQuery> ExtraQueries;
    // one query struct per directory split
    TVec<FileQuery> SymDirQueries(QuerySplit.Len()); // one filequery per querysplit
    for (int i = 0; i < QuerySplit.Len(); i++)
    {
        TStr dir_name = QuerySplit[i];
        if (QueryMap.IsKey(dir_name))
        {
            // query includes this directory name
            SymDirQueries[i] = QueryMap.GetDat(dir_name);
            QueryMap.DelKey(dir_name); // remove this query from the map
        }
        else
        {
            // this is empty, so we set it as an empty string
            SymDirQueries[i].QueryName = dir_name;
            SymDirQueries[i].QueryVal = TStrV();
        }
    }

    QueryCollector qCollector(Query, &Schema);
    // retrieve the data and put into an executable
    UnravelQuery(SymDirQueries, 0, OutputDir, QueryMap, qCollector,
                 InitialTimeStamp, FinalTimeStamp);
    std::cout << "fully unravelled query" << std::endl;
    qCollector.ConvertToTimeCollection(r, zeroFlag);
    if (OutputDir.Len() != 0)
    {
        TFOut outstream(OutputFile);
        SaveQuerySet(r, outstream);
    }
}

void TSTimeSymDir::UnravelQuery(TVec<FileQuery> &SymDirQueries, int SymDirQueryIndex,
                                TStr &Dir, THash<TStr, FileQuery> &ExtraQueries,
                                QueryCollector &qCollector, TStr &InitialTimeStamp,
                                TStr &FinalTimeStamp)
{

    if (SymDirQueryIndex == QuerySplit.Len())
    {
        // base case: done traversing the symbolic directory, so we are in a directory
        // of pure event files. gather these event files into r
        GatherQueryResult(Dir, ExtraQueries, qCollector, InitialTimeStamp, FinalTimeStamp);
        return;
    }
    if (SymDirQueries[SymDirQueryIndex].QueryVal.Len() != 0)
    {
        // if this directory has a query value, go to that folder
        for (int i = 0; i < SymDirQueries[SymDirQueryIndex].QueryVal.Len(); i++)
        {
            TStr val = SymDirQueries[SymDirQueryIndex].QueryVal[i];
            TStr path = Dir + TStr("/") + TTimeFFile::EscapeFileName(val);
            if (TDir::Exists(path))
            {
                UnravelQuery(SymDirQueries, SymDirQueryIndex + 1, path, ExtraQueries,
                             qCollector, InitialTimeStamp, FinalTimeStamp);
            }
        }
    }
    else
    {
        // this directory doesn't have a query value, so queue up gathering in all subfolders
        TStrV FnV;
        TTimeFFile::GetAllFiles(Dir, FnV);
        for (int i = 0; i < FnV.Len(); i++)
        {
            UnravelQuery(SymDirQueries, SymDirQueryIndex + 1, FnV[i], ExtraQueries,
                         qCollector, InitialTimeStamp, FinalTimeStamp);
        }
    }
}

void TSTimeSymDir::GatherQueryResult(TStr FileDir, THash<TStr, FileQuery> &ExtraQueries,
                                     QueryCollector &qCollector, TStr &InitialTimeStamp,
                                     TStr &FinalTimeStamp)
{
    // Get bounding timestamps
    TTime initTS = 0;
    if (InitialTimeStamp.Len() != 0)
    {
        initTS = Schema.ConvertTime(InitialTimeStamp);
    }
    TTime finalTS = TTime::Mx;
    if (FinalTimeStamp.Len() != 0)
    {
        finalTS = Schema.ConvertTime(FinalTimeStamp);
    }

    TStrV FnV;
    TTimeFFile::GetAllFiles(FileDir, FnV);
    for (int i = 0; i < FnV.Len(); i++)
    {
        TStr FileName = FnV[i];
        TFIn inputstream(FileName);
        TPt<TSTime> t = TSTime::LoadSTime(inputstream, false);
        THash<TStr, FileQuery>::TIter it;
        bool validQuery = true;
        for (it = ExtraQueries.BegI(); it != ExtraQueries.EndI(); it++)
        {
            TStr QueryName = it.GetKey();
            TStrV QueryVal = it.GetDat().QueryVal;
            AssertR(Schema.KeyNamesToIndex.IsKey(QueryName), "Invalid query"); // QueryName needs to exist
            TInt IdIndex = Schema.KeyNamesToIndex.GetDat(QueryName);
            if (!QueryVal.IsIn(t->KeyIds[IdIndex]))
            {
                validQuery = false;
                break; // does not match query
            }
        }
        if (validQuery)
        {
            t->LoadData(inputstream);
            t->TruncateVectorByTime(initTS, finalTS); // todo make query faster, make multiple ts
            qCollector.AddSTimeToCollector(t);
        }
    }
}

// fill in a hash from the query name to the actual query
void TSTimeSymDir::GetQuerySet(TVec<FileQuery> &Query, THash<TStr, FileQuery> &result)
{
    for (int i = 0; i < Query.Len(); i++)
    {
        result.AddDat(Query[i].QueryName, Query[i]);
    }
}

//--------
// Creating symbolic directory
void TSTimeSymDir::CreateSymbolicDirs()
{
    if (FileSysCreated)
        return;
    TraverseEventFiles(InputDir);
    FileSysCreated = true;
}

void TSTimeSymDir::TraverseEventFiles(TStr &Dir)
{
    if (!TDir::Exists(Dir))
    {
        // this is the event file
        CreateSymDirsForEventFile(Dir);
    }
    else
    {
        TStrV FnV;
        TTimeFFile::GetAllFiles(Dir, FnV); // get the directories
        for (int i = 0; i < FnV.Len(); i++)
        {
            TraverseEventFiles(FnV[i]);
        }
    }
}

void TSTimeSymDir::CreateSymDirsForEventFile(TStr &EventFileName)
{
    TFIn inputstream(EventFileName);
    TPt<TSTime> t = TSTime::LoadSTime(inputstream, false);
    TStrV SymDirs;
    // find the dir names
    for (int i = 0; i < QuerySplit.Len(); i++)
    {
        TStr &Query = QuerySplit[i];
        AssertR(Schema.KeyNamesToIndex.IsKey(Query), "Query to split on SymDir not found");
        TInt IDIndex = Schema.KeyNamesToIndex.GetDat(Query);
        SymDirs.Add(TTimeFFile::EscapeFileName(t->KeyIds[IDIndex]));
    }
    TStr path = OutputDir;
    for (int i = 0; i < SymDirs.Len(); i++)
    {
        path = path + TStr("/") + SymDirs[i];
        if (!TDir::Exists(path))
        {
            std::cout << "creating directory " << path.CStr() << std::endl;
            AssertR(TDir::GenDir(path), "Could not create directory");
        }
    }
    // create a sym link at the end of the path for this stime
    TStr final_path = path + TStr("/") + TCSVParse::CreateIDVFileName(t->KeyIds);
    char *real_event_path = realpath(EventFileName.CStr(), NULL);
    int success = symlink(real_event_path, final_path.CStr());
    free(real_event_path);
    AssertR(success != -1, "Failed to create symbolic directory");
}

void GetEventFileList(TStr &Dir, TStrV &Files)
{
    if (!TDir::Exists(Dir))
    {
        Files.Add(Dir);
    }
    else
    {
        TStrV FnV;
        TTimeFFile::GetAllFiles(Dir, FnV); // get the directories
        for (int i = 0; i < FnV.Len(); i++)
        {
            GetEventFileList(FnV[i], Files);
        }
    }
}

void SummaryStats(TStr &RawDir, TStr &SchemaFile, TStr &OutputFile)
{
    TSchema Schema(SchemaFile);
    TStrV EventFileList;
    GetEventFileList(RawDir, EventFileList);
    TVec<TStrV> rows;
    for (int i = 0; i < EventFileList.Len(); i++)
    {
        TFIn inputstream(EventFileList[i]);
        TPt<TSTime> t = TSTime::LoadSTime(inputstream, true);
        TStrV row = t->KeyIds;
        TInt length = t->Len();
        TTime t_zero = t->DirectAccessTime(0);
        TTime t_last = t->DirectAccessTime(length - 1);
        TStr t_zero_str = Schema.ConvertTimeToStr(t_zero);
        TStr t_last_str = Schema.ConvertTimeToStr(t_last);
        row.Add(t_zero_str);	  // start time
        row.Add(t_last_str);	  // end time
        row.Add(length.GetStr()); // number of values
        rows.Add(row);
    }
    rows.Sort();
    TFOut outstream(OutputFile);

    for (int i = 0; i < rows.Len(); i++)
    {
        for (int j = 0; j < rows[i].Len(); j++)
        {
            outstream.PutStr(rows[i][j]);
            outstream.PutCh(',');
        }
        outstream.PutLn();
    }
    // Go through each ID
}

/*
 * ------------------ Query Collector -------------------
 */
QueryCollector::QueryCollector(TVec<FileQuery> Query, TSchema *_schema) : QueryCompute(Query.Len()), QueryGrid()
{
    std::cout << "creating collector " << std::endl;
    schema = _schema;
    // Create indexer
    TInt currDepth = 1;
    for (int i = Query.Len() - 1; i >= 0; i--)
    {
        FileQuery &fq = Query[i];
        TInt index;
        AssertR(schema->KeyNamesToIndex.IsKeyGetDat(fq.QueryName, index),
                "Query contains QueryName not in Schema");
        THash<TStr, TInt> innerHash;
        for (int j = 0; j < fq.QueryVal.Len(); j++)
        {
            innerHash.AddDat(fq.QueryVal[j], j); // Map QueryValue into index
        }
        QueryCompute[i] = {index, innerHash, fq.QueryVal, currDepth};
        currDepth = currDepth * fq.QueryVal.Len();
    }
    QueryGrid.Gen(currDepth); // create a vector with maximum depth
}

void QueryCollector::ConvertToTimeCollection(TTimeCollection &r, bool ZeroFlag)
{
    for (int i = 0; i < QueryGrid.Len(); i++)
    {
        if (QueryGrid[i].Len() > 0)
        {
            r.TimeCollection.AddV(QueryGrid[i]);
        }
        else if (ZeroFlag)
        {
            r.TimeCollection.Add(ConstructEmptyTSTime(i));
        }
    }
}

TPt<TSTime> QueryCollector::ConstructEmptyTSTime(TInt index)
{
    TStrV DummyKeyIds(schema->KeyNames.Len());
    for (int i = 0; i < QueryCompute.Len(); i++)
    {
        QueryIndexer &qi = QueryCompute[i];
        int keyIndex = index / qi.Depth; // this is the QueryVal index that this was supposed to correspond to
        index = index - keyIndex * qi.Depth;
        DummyKeyIds[qi.QueryNameIndex] = qi.QueryVals[keyIndex]; // fill in the dummy index
    }
    TType t = schema->GetType(DummyKeyIds);
    return TSTime::TypedTimeGenerator(t, DummyKeyIds);
}

void QueryCollector::AddSTimeToCollector(TPt<TSTime> elem)
{
    int idx = 0;
    for (int i = 0; i < QueryCompute.Len(); i++)
    {
        QueryIndexer &qi = QueryCompute[i];
        TStr id = elem->KeyIds[qi.QueryNameIndex];
        TInt innerIdx = 0;
        AssertR(qi.QueryValIndex.IsKeyGetDat(id, innerIdx),
                "an incorrect result was added to the query object");
        idx += innerIdx * qi.Depth;
    }
    QueryGrid[idx].Add(elem);
}
