/*
 * (C) Copyright 2022 NOAA/NWS/NCEP/EMC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0
 * which can be obtained at http://www.apache.org/licenses/LICENSE-2.0.
 */

#include "ResultSet.h"

#include "eckit/exception/Exceptions.h"

#include <algorithm>
#include <string>
#include <iostream>

#ifdef BUILD_PYTHON_BINDING
    #include <time.h>
    #include <pybind11/pybind11.h>
    #include <pybind11/numpy.h>
    #include <pybind11/stl.h>

    namespace py = pybind11;
#endif

#include "Constants.h"
#include "VectorMath.h"


namespace Ingester {
namespace bufr {
    ResultSet::ResultSet(const std::vector<std::string>& names) :
      names_(names)
    {
        fieldWidths.resize(names.size());
    }

    ResultSet::~ResultSet()
    {
    }

    std::shared_ptr<Ingester::DataObjectBase>
        ResultSet::get(const std::string& fieldName,
                       const std::string& groupByFieldName,
                       const std::string& overrideType) const
    {
        if (dataFrames_.empty())
        {
            throw eckit::BadValue("This ResultSet is empty (doesn't contain any data).");
        }

        if (!dataFrames_[0].hasFieldNamed(fieldName))
        {
            throw eckit::BadValue("This ResultSet does not contain a field named " +
                                  fieldName);
        }

        if (!groupByFieldName.empty() && !dataFrames_[0].hasFieldNamed(groupByFieldName))
        {
            throw eckit::BadValue("This ResultSet does not contain a field named " +
                                  groupByFieldName);
        }

        std::vector<double> data;
        std::vector<int> dims;
        std::vector<Query> dimPaths;
        TypeInfo info;

        getRawValues(fieldName,
                     groupByFieldName,
                     data,
                     dims,
                     dimPaths,
                     info);

        std::shared_ptr<Ingester::DataObjectBase> object = makeDataObject(fieldName,
                                                                          groupByFieldName,
                                                                          info,
                                                                          overrideType,
                                                                          data,
                                                                          dims,
                                                                          dimPaths);

        return object;
    }


#ifdef BUILD_PYTHON_BINDING
        py::array ResultSet::getNumpyArray(const std::string& fieldName,
                                           const std::string& groupByFieldName,
                                           const std::string& overrideType) const
        {
            auto dataObj = get(fieldName, groupByFieldName, overrideType);
            return dataObj->getNumpyArray();
        }

        py::array ResultSet::getNumpyDatetimeArray(const std::string& year,
                                                   const std::string& month,
                                                   const std::string& day,
                                                   const std::string& hour,
                                                   const std::string& minute,
                                                   const std::string& second,
                                                   const std::string& groupBy) const
        {
            std::shared_ptr<DataObjectBase> yearObj = get(year, groupBy);
            std::shared_ptr<DataObjectBase> monthObj = get(month, groupBy);
            std::shared_ptr<DataObjectBase> dayObj = get(day, groupBy);
            std::shared_ptr<DataObjectBase> hourObj = get(hour, groupBy);

            std::shared_ptr<DataObjectBase> minuteObj = nullptr;
            std::shared_ptr<DataObjectBase> secondObj = nullptr;

            if (!minute.empty())
            {
                minuteObj = get(minute, groupBy);
            }

            if (!second.empty())
            {
                secondObj = get(second, groupBy);
            }

            // make strides array
            std::vector<ssize_t> strides(yearObj->getDims().size());
            strides[0] = sizeof(int64_t);
            for (size_t i = 1; i < yearObj->getDims().size(); ++i)
            {
                strides[i] = sizeof(int64_t) * yearObj->getDims()[i];
            }

            auto array = py::array(py::dtype("datetime64[s]"), yearObj->getDims(), strides);
            auto arrayPtr = static_cast<int64_t*>(array.mutable_data());

            for (size_t i = 0; i < yearObj->size(); ++i)
            {
                std::tm time;
                time.tm_year = yearObj->getAsInt(i) - 1900;
                time.tm_mon = monthObj->getAsInt(i) - 1;
                time.tm_mday = dayObj->getAsInt(i);
                time.tm_hour = hourObj->getAsInt(i);
                time.tm_min = minuteObj ? minuteObj->getAsInt(i) : 0;
                time.tm_sec = secondObj ? secondObj->getAsInt(i) : 0;
                time.tm_isdst = 0;

                arrayPtr[i] = static_cast<int64_t>(timegm(&time));
            }

            // Create the mask array
            py::object numpyModule = py::module::import("numpy");

            // Create the mask array
            py::array_t<bool> mask(yearObj->getDims());
            bool* maskPtr = static_cast<bool*>(mask.mutable_data());
            for (size_t idx = 0; idx < yearObj->size(); idx++)
            {
                maskPtr[idx] = yearObj->isMissing(idx) |
                               monthObj->isMissing(idx) |
                               dayObj->isMissing(idx) |
                               hourObj->isMissing(idx) |
                               (minuteObj ? minuteObj->isMissing(idx) : false) |
                               (secondObj ? secondObj->isMissing(idx) : false);
            }

            // Create a masked array from the data and mask arrays
            py::array maskedArray = numpyModule.attr("ma").attr("masked_array")(array, mask);
            numpyModule.attr("ma").attr("set_fill_value")(maskedArray, 0);

            return maskedArray;
        }
#endif


    DataFrame& ResultSet::nextDataFrame()
    {
        dataFrames_.push_back(DataFrame(names_.size()));
        return dataFrames_.back();
    }

    void ResultSet::getRawValues(const std::string& fieldName,
                                 const std::string& groupByField,
                                 std::vector<double>& data,
                                 std::vector<int>& dims,
                                 std::vector<Query>& dimPaths,
                                 TypeInfo& info) const
    {
        // Find the dims based on the largest sequence counts in the fields
        // Compute Dims
        std::vector<int> dimsList;
        std::vector<int> exportDims;
        int groupbyIdx = 0;
        int totalGroupbyElements = 0;

        int groupByFieldIdx = 0;
        int targetFieldIdx = dataFrames_[0].fieldIndexForNodeNamed(fieldName);

        if (groupByField != "")
        {
            groupByFieldIdx = dataFrames_[0].fieldIndexForNodeNamed(groupByField);

            // Validate that the groupByField and the targetField share a common path
            auto &groupByFieldElement = dataFrames_[0].fieldAtIdx(groupByFieldIdx);
            auto &groupByPath = groupByFieldElement.target->dimPaths.back();
            auto &targetPath =
                dataFrames_[0].fieldAtIdx(targetFieldIdx).target->dimPaths.back();
            auto groupByPathComps = splitPath(groupByPath.str());
            auto targetPathComps = splitPath(targetPath.str());

            for (size_t i = 1;
                 i < std::min(groupByPathComps.size(), targetPathComps.size());
                 i++)
            {
                if (targetPathComps[i] != groupByPathComps[i])
                {
                    std::ostringstream errStr;
                    errStr << "The GroupBy and Target Fields do not share a common path.\n";
                    errStr << "GroupByField path: " << groupByPath.str() << std::endl;
                    errStr << "TargetField path: " << targetPath.str() << std::endl;
                    throw eckit::BadParameter(errStr.str());
                }
            }
        }

        auto& targetField = dataFrames_[0].fieldAtIdx(targetFieldIdx);
        dimPaths = targetField.target->dimPaths;
        exportDims = targetField.target->exportDimIdxs;

        for (auto& dataFrame : dataFrames_)
        {
            auto& targetField = dataFrame.fieldAtIdx(targetFieldIdx);
            if (!targetField.target->dimPaths.empty() &&
                dimPaths.size() < targetField.target->dimPaths.size())
            {
                dimPaths = targetField.target->dimPaths;
                exportDims = targetField.target->exportDimIdxs;
            }

            size_t dimsLen = targetField.seqCounts.size();
            if (dimsList.size() < dimsLen)
            {
                dimsList.resize(dimsLen, 0);
            }

            for (size_t cntIdx = 0; cntIdx < targetField.seqCounts.size(); ++cntIdx)
            {
                if (!targetField.seqCounts[cntIdx].empty())
                {
                    dimsList[cntIdx] = std::max(dimsList[cntIdx],
                                                max(targetField.seqCounts[cntIdx]));
                }
            }

            info.reference = std::min(info.reference, targetField.target->typeInfo.reference);
            info.bits = std::max(info.bits, targetField.target->typeInfo.bits);

            if (std::abs(targetField.target->typeInfo.scale) > info.scale)
            {
                info.scale = targetField.target->typeInfo.scale;
            }

            if (info.unit.empty()) info.unit = targetField.target->typeInfo.unit;

            if (groupByField != "")
            {
                auto& groupByField = dataFrame.fieldAtIdx(groupByFieldIdx);
                groupbyIdx = std::max(groupbyIdx, static_cast<int>(groupByField.seqCounts.size()));

                if (groupbyIdx > static_cast<int>(dimsList.size()))
                {
                    dimPaths = {groupByField.target->dimPaths.back()};

                    int groupbyElementsForFrame = 1;
                    for (auto &seqCount : groupByField.seqCounts)
                    {
                        if (!seqCount.empty())
                        {
                            groupbyElementsForFrame *= max(seqCount);
                        }
                    }

                    totalGroupbyElements = std::max(totalGroupbyElements, groupbyElementsForFrame);
                }
                else
                {
                    dimPaths = {};
                    for (size_t targetIdx = groupByField.target->exportDimIdxs.size() - 1;
                         targetIdx < targetField.target->dimPaths.size();
                         ++targetIdx)
                    {
                        dimPaths.push_back(targetField.target->dimPaths[targetIdx]);
                    }
                }
            }
        }

        auto allDims = dimsList;

        // If there is absolutely no data for a field you will have the problem were the
        // size of some dimensions are zero. We need to have at least 1 element in each
        // dimension to make room for the missing value. This if statement makes sure there
        // is at least 1 element in each dimension.
        for (size_t dimIdx = 0; dimIdx < allDims.size(); ++dimIdx)
        {
            if (allDims[dimIdx] == 0)
            {
                allDims[dimIdx] = 1;
            }
        }

        if (groupbyIdx > 0)
        {
            // The groupby field occurs at the same or greater repetition level as the target field.
            if (groupbyIdx > static_cast<int>(dimsList.size()))
            {
                dims.resize(1, totalGroupbyElements);
                exportDims = {0};
                allDims = dims;
            }
            // The groupby field occurs at a lower repetition level than the target field.
            else
            {
                dims.resize(dimsList.size() - groupbyIdx + 1, 1);
                for (auto dimIdx = 0; dimIdx < groupbyIdx; ++dimIdx)
                {
                    dims[0] *= allDims[dimIdx];
                }

                for (size_t dimIdx = groupbyIdx; dimIdx < allDims.size(); ++dimIdx)
                {
                    dims[dimIdx - groupbyIdx + 1] = allDims[dimIdx];
                }

                exportDims = exportDims - (groupbyIdx - 1);

                // Filter out exportDims that are 0
                std::vector<int> filteredExportDims;
                for (size_t dimIdx = 0; dimIdx < exportDims.size(); ++dimIdx)
                {
                    if (exportDims[dimIdx] >= 0)
                    {
                        filteredExportDims.push_back(exportDims[dimIdx]);
                    }
                }

                if (filteredExportDims.empty() || filteredExportDims[0] != 0)
                {
                    filteredExportDims.insert(filteredExportDims.begin(), 0);
                }

                exportDims = filteredExportDims;
            }
        }
        else
        {
            dims = allDims;
        }

        size_t totalRows = dims[0] * dataFrames_.size();

        // Make data set
        int rowLength = 1;
        for (size_t dimIdx = 1; dimIdx < dims.size(); ++dimIdx)
        {
            rowLength *= dims[dimIdx];
        }

        data.resize(totalRows * rowLength, MissingValue);
        for (size_t frameIdx = 0; frameIdx < dataFrames_.size(); ++frameIdx)
        {
            auto& dataFrame = dataFrames_[frameIdx];
            std::vector<std::vector<double>> frameData;
            auto& targetField = dataFrame.fieldAtIdx(targetFieldIdx);

            if (!targetField.data.empty()) {
                getRowsForField(targetField,
                                frameData,
                                allDims,
                                groupbyIdx);

                auto dataRowIdx = dims[0] * frameIdx;
                for (size_t rowIdx = 0; rowIdx < frameData.size(); ++rowIdx)
                {
                    auto &row = frameData[rowIdx];
                    for (size_t colIdx = 0; colIdx < row.size(); ++colIdx)
                    {
                        data[dataRowIdx*rowLength + rowIdx * row.size() + colIdx] = row[colIdx];
                    }
                }
            }
        }

        // Convert dims per data frame to dims for all the collected data.
        dims[0] = totalRows;
        dims = slice(dims, exportDims);
    }

//    subroutine result_set__get_rows_for_field(self, target_field, data_rows, dims, groupby_idx)

    void ResultSet::getRowsForField(const DataField& targetField,
                                    std::vector<std::vector<double>>& dataRows,
                                    const std::vector<int>& dims,
                                    int groupbyIdx) const
    {
        size_t maxCounts = 0;
        std::vector<size_t> idxs(targetField.data.size());
        for (size_t i = 0; i < idxs.size(); ++i)
        {
            idxs[i] = i;
        }

        // Compute max counts
        for (size_t i = 0; i < targetField.seqCounts.size(); ++i)
        {
            if (maxCounts < targetField.seqCounts[i].size())
            {
                maxCounts = targetField.seqCounts[i].size();
            }
        }

        // Compute insert array
        std::vector<std::vector<int>> inserts(dims.size(), {0});
        for (size_t repIdx = 0;
             repIdx < std::min(dims.size(), targetField.seqCounts.size());
             ++repIdx)
        {
            inserts[repIdx] = product<int>(dims.begin() + repIdx, dims.end()) -
                              targetField.seqCounts[repIdx] *
                              product<int>(dims.begin() + repIdx + 1, dims.end());
        }

        // Inflate the data, compute the idxs for each data element in the result array
        for (int dim_idx = dims.size() - 1; dim_idx >= 0; --dim_idx)
        {
            for (size_t insert_idx = 0; insert_idx < inserts[dim_idx].size(); ++insert_idx)
            {
                size_t num_inserts = inserts[dim_idx][insert_idx];
                if (num_inserts > 0)
                {
                    int data_idx = product<int>(dims.begin() + dim_idx, dims.end()) *
                            insert_idx + product<int>(dims.begin() + dim_idx, dims.end())
                                    - num_inserts - 1;

                    for (size_t i = 0; i < idxs.size(); ++i)
                    {
                        if (static_cast<int>(idxs[i]) > data_idx)
                        {
                            idxs[i] += num_inserts;
                        }
                    }
                }
            }
        }

        auto output = std::vector<double>(product(dims), MissingValue);
        for (size_t i = 0; i < idxs.size(); ++i)
        {
            output[idxs[i]] = targetField.data[i];
        }

        // Apply groupBy and make output
        if (groupbyIdx > 0)
        {
            if (groupbyIdx > static_cast<int>(targetField.seqCounts.size()))
            {
                size_t numRows = product(dims);
                dataRows.resize(numRows * maxCounts, {MissingValue});
                for (size_t i = 0; i < numRows; ++i)
                {
                    if (output.size())
                    {
                        dataRows[i][0] = output[0];
                    }
                }
            }
            else
            {
                size_t numRows = product<int>(dims.begin(), dims.begin() + groupbyIdx);
                std::vector<int> rowDims;
                rowDims.assign(dims.begin() + groupbyIdx, dims.end());

                size_t numsPerRow = static_cast<size_t>(product(rowDims));
                dataRows.resize(numRows, std::vector<double>(numsPerRow, MissingValue));
                for (size_t i = 0; i < numRows; ++i)
                {
                    for (size_t j = 0; j < numsPerRow; ++j)
                    {
                        dataRows[i][j] = output[i * numsPerRow + j];
                    }
                }
            }
        }
        else
        {
            dataRows.resize(1);
            dataRows[0] = output;
        }
    }

    std::string ResultSet::unit(const std::string& fieldName) const
    {
        auto fieldIdx = dataFrames_.front().fieldIndexForNodeNamed(fieldName);
        return dataFrames_.front().fieldAtIdx(fieldIdx).target->unit;
    }

    std::shared_ptr<DataObjectBase> ResultSet::makeDataObject(
                                const std::string& fieldName,
                                const std::string& groupByFieldName,
                                TypeInfo& info,
                                const std::string& overrideType,
                                const std::vector<double> data,
                                const std::vector<int> dims,
                                const std::vector<Query> dimPaths) const
    {
        std::shared_ptr<DataObjectBase> object;
        if (overrideType.empty())
        {
            object = objectByTypeInfo(info);
        }
        else
        {
            object = objectByType(overrideType);

            if ((overrideType == "string" && !info.isString()) ||
                (overrideType != "string" && info.isString()))
            {
                std::ostringstream errMsg;
                errMsg << "Conversions between numbers and strings are not currently supported. ";
                errMsg << "See the export definition for \"" << fieldName << "\".";
                throw eckit::BadParameter(errMsg.str());
            }
        }

        object->setData(data, 10e10);
        object->setDims(dims);
        object->setFieldName(fieldName);
        object->setGroupByFieldName(groupByFieldName);
        object->setDimPaths(dimPaths);

        return object;
    }

    std::shared_ptr<DataObjectBase> ResultSet::objectByTypeInfo(TypeInfo &info) const
    {
        std::shared_ptr<DataObjectBase> object;

        if (info.isString())
        {
            object = std::make_shared<DataObject<std::string>>();
        }
        else if (info.isInteger())
        {
            if (info.isSigned())
            {
                if (info.is64Bit())
                {
                    object = std::make_shared<DataObject<int64_t>>();
                }
                else
                {
                    object = std::make_shared<DataObject<int32_t>>();
                }
            }
            else
            {
                if (info.is64Bit())
                {
                    object = std::make_shared<DataObject<uint64_t>>();
                }
                else
                {
                    object = std::make_shared<DataObject<uint32_t>>();
                }
            }
        }
        else
        {
            if (info.is64Bit())
            {
                object = std::make_shared<DataObject<double>>();
            }
            else
            {
                object = std::make_shared<DataObject<float>>();
            }
        }

        return object;
    }

    std::shared_ptr<DataObjectBase> ResultSet::objectByType(const std::string& overrideType) const
    {
        std::shared_ptr<DataObjectBase> object;

        if (overrideType == "int" || overrideType == "int32")
        {
            object = std::make_shared<DataObject<int32_t>>();
        }
        else if (overrideType == "float" || overrideType == "float32")
        {
            object = std::make_shared<DataObject<float>>();
        }
        else if (overrideType == "double" || overrideType == "float64")
        {
            object = std::make_shared<DataObject<double>>();
        }
        else if (overrideType == "string")
        {
            object = std::make_shared<DataObject<std::string>>();
        }
        else if (overrideType == "int64")
        {
            object = std::make_shared<DataObject<int64_t>>();
        }
        else if (overrideType == "uint64")
        {
            object = std::make_shared<DataObject<uint64_t>>();
        }
        else if (overrideType == "uint32" || overrideType == "uint")
        {
            object = std::make_shared<DataObject<uint32_t>>();
        }
        else
        {
            std::ostringstream errMsg;
            errMsg << "Unknown or unsupported type " << overrideType << ".";
            throw eckit::BadParameter(errMsg.str());
        }

        return object;
    }

    std::vector<std::string> ResultSet::splitPath(const std::string& path)
    {
        std::vector<std::string> components;
        std::string::size_type start = 0;
        std::string::size_type end = 0;

        while ((end = path.find('/', start)) != std::string::npos)
        {
            if (end != start)
            {
                components.push_back(path.substr(start, end - start));
            }

            start = end + 1;
        }

        if (start < path.size())
        {
            components.push_back(path.substr(start));
        }

        return components;
    }
}  // namespace bufr
}  // namespace Ingester
