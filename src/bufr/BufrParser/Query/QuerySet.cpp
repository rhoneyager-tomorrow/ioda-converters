/*
 * (C) Copyright 2022 NOAA/NWS/NCEP/EMC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0
 * which can be obtained at http://www.apache.org/licenses/LICENSE-2.0.
 */

#include "QuerySet.h"

#include <algorithm>


namespace Ingester {
namespace bufr {

    QuerySet::QuerySet() :
        includesAllSubsets_(true),
        addHasBeenCalled_(false),
        limitSubsets_({}),
        presentSubsets_({})
    {
    }

    QuerySet::QuerySet(const std::vector<std::string>& subsets) :
        includesAllSubsets_(false),
        addHasBeenCalled_(false),
        limitSubsets_(std::set<std::string>(subsets.begin(),
                                            subsets.end())),
        presentSubsets_({})
    {
        if (limitSubsets_.empty())
        {
            includesAllSubsets_ = true;
        }
    }

    void QuerySet::add(const std::string& name, const std::string& queryStr)
    {
        if (!addHasBeenCalled_)
        {
            addHasBeenCalled_ = true;
            includesAllSubsets_ = false;
        }

        std::vector<Query> queries;
        for (const auto &query : QueryParser::parse(queryStr))
        {
            if (limitSubsets_.empty())
            {
                includesAllSubsets_ = query.subset->isAnySubset;
                presentSubsets_.insert(query.subset->name);
            }
            else
            {
                if (query.subset->isAnySubset)
                {
                    presentSubsets_ = limitSubsets_;
                }
                else
                {
                    presentSubsets_.insert(query.subset->name);

                    std::vector<std::string> newSubsets;
                    std::set_intersection(limitSubsets_.begin(),
                                          limitSubsets_.end(),
                                          presentSubsets_.begin(),
                                          presentSubsets_.end(),
                                          std::back_inserter(newSubsets));

                    presentSubsets_ = std::set<std::string>(newSubsets.begin(),
                                                            newSubsets.end());
                }
            }

            queries.emplace_back(query);
        }

        queryMap_[name] = queries;
    }

    bool QuerySet::includesSubset(const std::string& subset) const
    {
        bool includesSubset = true;
        if (!includesAllSubsets_)
        {
            if (queryMap_.empty())
            {
                includesSubset = (limitSubsets_.find(subset) != limitSubsets_.end());
            }
            else
            {
                includesSubset = (presentSubsets_.find(subset) != presentSubsets_.end());
            }
        }

        return includesSubset;
    }

    std::vector<std::string> QuerySet::names() const
    {
        std::vector<std::string> names;
        for (auto const& query : queryMap_)
        {
            names.push_back(query.first);
        }

        return names;
    }

    std::vector<Query> QuerySet::queriesFor(const std::string& name) const
    {
        return queryMap_.at(name);
    }

}  // namespace bufr
}  // namespace Ingester
