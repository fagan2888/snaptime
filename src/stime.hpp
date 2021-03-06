#ifndef TSTIME_H
#define TSTIME_H

#include "stime_protos.hpp"

// Intermediary framework unit. Identifiable by a list of IDs (who's ordering)
// is defined by a global schema) and a TVec of time to value pairs,
// but not sorted. Values are also just strings
class TUnsortedTime
{
  public:
    TStrV KeyIds;            // Vector of key ids
    TVec<TRawData> TimeData; // just a list of time values
  public:
    TUnsortedTime() : KeyIds(), TimeData() {}
    TUnsortedTime(TStrV &ids) : KeyIds(ids), TimeData() {}
    TUnsortedTime(TStrV ids, TVec<TRawData> t_data) : KeyIds(ids), TimeData(t_data) {}
    void Save(TSOut &SOut)
    {
        KeyIds.Save(SOut);
        TimeData.Save(SOut);
    }
    void Load(TSIn &SIn)
    {
        KeyIds.Clr();
        TimeData.Clr();
        KeyIds.Load(SIn);
        TimeData.Load(SIn);
    }
};

// An abstract class containing converted and sorted data
// Specific implementations are included in TSTypedTime
class TSTime
{
  public:
    TType Type;
    TStrV KeyIds;
#ifndef SWIG
    TCRef CRef; // for keeping track of pointers
#endif
  public:
    TSTime() {}
    TSTime(TType type, TStrV &key_ids) : Type(type), KeyIds(key_ids), CRef() {}
    virtual ~TSTime() = 0;
#ifndef SWIG
    TSTime(const TSTime &t) = delete; // cannot copy
#endif
    // Given an unsorted time, convert each element in the unsorted time and add
    // it to our TimeData vector.
    virtual void AddUnsortedTime(TUnsortedTime &RawData) = 0;

    // Sort the data based on timestamp
    virtual void Sort() = 0;

    // Generate an empty TSTime of the given type. Return a pointer
    static TPt<TSTime> TypedTimeGenerator(TType type, TStrV &key_ids);

    // Load a TSTime from an input stream. If ShouldLoadData is false,
    // only load the type and key id.
    static TPt<TSTime> LoadSTime(TSIn &SIn, bool ShouldLoadData = true);
    virtual void Save(TSOut &SOut) = 0;

    // Load just the TimeData vector into the input
    virtual void LoadData(TSIn &SIn) = 0;
    // Save just the TimeData vector into the output
    virtual void SaveData(TSOut &SOut) = 0;

    // Return the TTime at the given index
    virtual TTime DirectAccessTime(int i) = 0;
    // Return a pointer to the value at the given index (TODO: fix, this is a hack)
    virtual void *DirectAccessValue(int i) = 0;
    // Return the length of the time data vector
    virtual TInt Len() = 0;
    // truncate the vector to be bounded by firstTime and lastTimes
    virtual void TruncateVectorByTime(TTime firstTime, TTime lastTime) = 0;
    //Find the index of the largest value with timestamp < t
    virtual int GetFirstValueWithTime(TTime t) = 0;
    //Find the index of the smallest value with timestamp > t
    virtual int GetLastValueWithTime(TTime t) = 0;

    // Typed getters
    TBool GetBool(int i);
    TFlt GetFloat(int i);
    TInt64 GetInt(int i);
    TStr GetStr(int i);

    void DebugPrint();
};

// TSTypedType is a per type implementation of TSTime
template <typename TVal>
class TSTypedTime : public TSTime
{
  public:
    TVec<TPair<TTime, TVal>> TimeData;
    std::function<TVal(TStr &)> ConvertString; // TODO: does not set after load

    TSTypedTime(TType type, TStrV &key_ids, std::function<TVal(TStr &)> ConvertString_) : TSTime(type, key_ids),
                                                                                          TimeData(), ConvertString(ConvertString_) {}

    ~TSTypedTime() {}

    void SaveData(TSOut &SOut) { TimeData.Save(SOut); }
    void LoadData(TSIn &SIn) { TimeData.Load(SIn); }

    void AddUnsortedTime(TUnsortedTime &RawData)
    {
        for (int i = 0; i < RawData.TimeData.Len(); i++)
        {
            TRawData &raw_data_val = RawData.TimeData[i];
            TimeData.Add({raw_data_val.Val1, ConvertString(raw_data_val.Val2)});
        }
    }
    void Sort() { TimeData.Sort(); }

    TTime DirectAccessTime(int i)
    {
        AssertR(i < TimeData.Len(), "i is out of bounds for this STime");
        return TimeData[i].Val1;
    }

    void *DirectAccessValue(int i)
    {
        AssertR(i < TimeData.Len(), "i is out of bounds for this STime");
        return &(TimeData[i].Val2);
    }

    TInt Len() { return TimeData.Len(); }

    void Save(TSOut &SOut)
    {
        SOut.Save(static_cast<int>(Type));
        KeyIds.Save(SOut);
        TimeData.Save(SOut);
    }

    void TruncateVectorByTime(TTime firstTime, TTime lastTime)
    {
        int firstIndex = GetFirstValueWithTime(firstTime);
        int lastIndex = GetLastValueWithTime(lastTime);
        TVec<TPair<TTime, TVal>> TimeDataTrunc;
        TimeData.GetSubValV(firstIndex, lastIndex, TimeDataTrunc);
        TimeData = TimeDataTrunc;
    }

    // Find the index of the largest value with timestamp < t
    // If all values in TimeData are > timestamp, then return 0
    int GetFirstValueWithTime(TTime t)
    {
        int l = 0;
        int r = TimeData.Len() - 1;
        while (l < r)
        {
            int m = (l + r + 1) / 2;
            if (TimeData[m].Val1 <= t)
            {
                l = m;
            }
            else
            {
                r = m - 1;
            }
        }
        return l;
    }

    // Find the index of the smallest value with timestamp > t
    // If all values in TimeData are < t, return index of the last element
    int GetLastValueWithTime(TTime t)
    {
        int l = 0;
        int r = TimeData.Len() - 1;
        while (l < r)
        {
            int m = (l + r) / 2;
            if (TimeData[m].Val1 < t)
            {
                l = m + 1;
            }
            else
            {
                r = m;
            }
        }
        return l;
    }
};

class TTimeCollection
{
  public:
    TVec<TPt<TSTime>> TimeCollection;

  public:
    TTimeCollection() : TimeCollection() {}
    void Add(TPt<TSTime> t)
    {
        TimeCollection.Add(t);
    }
    int Len() { return TimeCollection.Len(); }
    TStrV GetIds(int rowNum) { return TimeCollection[rowNum]->KeyIds; }
    TType GetType(int rowNum) { return TimeCollection[rowNum]->Type; }
    TTime GetTime(int rowNum, int elemNum) { return TimeCollection[rowNum]->DirectAccessTime(elemNum); }
    TBool GetBool(int rowNum, int elemNum) { return TimeCollection[rowNum]->GetBool(elemNum); }
    TFlt GetFloat(int rowNum, int elemNum) { return TimeCollection[rowNum]->GetFloat(elemNum); }
    TInt64 GetInt(int rowNum, int elemNum) { return TimeCollection[rowNum]->GetInt(elemNum); }
    TStr GetStr(int rowNum, int elemNum) { return TimeCollection[rowNum]->GetStr(elemNum); }
    int GetSTimeLen(int rowNum) { return TimeCollection[rowNum]->Len(); }
};

#endif
