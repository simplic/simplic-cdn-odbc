#include "stdafx.h"
#include "Statement.h"
#include "OdbcTypeConverter.h"
#include "odbc_api.h"
#include "Helper.h"

#include <sqlext.h>

Statement::Statement(DbConnection* conn)
{
	m_connection = conn;
	m_activeRowDescriptor = &m_implicitRowDescriptor;
	m_activeParameterDescriptor = &m_implicitParameterDescriptor;
	m_isDownloadPending = false;
	m_downloadColumn = 0;

	m_isUploadPending = false;
	m_currentParameter = 0;
}


Statement::~Statement()
{
	m_connection->removeStatement(this);
}


SQLRETURN Statement::downloadBinaryChunk(
	SQLUSMALLINT nColumn,
	SQLPOINTER buffer,
	SQLLEN bufferSize,
	SQLLEN* strLenIndicator)
{
	// if we are already downloading something else, abort that download first!
	if (m_isDownloadPending && nColumn != m_downloadColumn)
	{
		abortBinaryTransfer();
	}

	// if we try to get data from a download that has already completed, return SQL_NO_DATA.
	if (!m_isDownloadPending && nColumn == m_downloadColumn)
	{
		*strLenIndicator = 0;
		return SQL_NO_DATA;
	}

	// if we are starting a new download, we need to set it up first.
	if (!m_isDownloadPending)
	{
		const Json::Value & value = (*m_currentResult.rows()[0])[nColumn - 1];
		bool ok = m_connection->beginDownload(value.asString());
		if (!ok) return SQL_ERROR;

		m_isDownloadPending = true;
		m_downloadColumn = nColumn;
	}

	// download the next (or first) chunk
	bool isCompleted = false;
	int64_t bytesRead = m_connection->downloadChunk(buffer, bufferSize, &isCompleted);

	if (bytesRead < 0)
	{
		abortBinaryTransfer();
		return SQL_ERROR;
	}

	if (isCompleted) m_isDownloadPending = false;


	/* TODO: Return SQL_SUCCESS_WITH_INFO with SQLSTATE 01004 and diag message "Binary data, right truncated"
	*       unless this was the last chunk of the download */
	if(strLenIndicator) *strLenIndicator = isCompleted ? bytesRead : SQL_NO_TOTAL;
	return SQL_SUCCESS;
}

void Statement::abortBinaryTransfer()
{
	m_isDownloadPending = false;
	m_isUploadPending = false;
	m_downloadColumn = 0;
	m_connection->resetTransfer();
}

void Statement::resetParameterUploadState()
{
	abortBinaryTransfer();
	m_currentParameter = 0;
	m_jsonParameters.clear();
	m_currentJsonParameter.clear();
}

SQLRETURN Statement::processParametersAndExecute()
{
	if (m_currentParameter == 0)
	{
		m_isFirstParamUpload = true;
	}

	++m_currentParameter;
	// iterate through the remaining parameters until we hit a data-at-execution parameter.
	while (m_activeParameterDescriptor->getItem(m_currentParameter, m_currentParameterDescriptor))
	{
		SQLLEN* indicator = m_currentParameterDescriptor.getIndicatorPointer();
		if (indicator == NULL)
		{
			resetParameterUploadState();
			return SQL_ERROR;
		}

		m_currentJsonParameter["name"] = NULL;
		m_currentJsonParameter["value"] = NULL;
		m_currentJsonParameter["type"] = m_currentParameterDescriptor.getTargetType();
		
		if (*indicator == SQL_DATA_AT_EXEC || *indicator <= SQL_LEN_DATA_AT_EXEC_OFFSET)
		{
			// data-at-execution parameter: we need to wait for the application to submit
			// the data via SQLParamData / SQLPutData
			return SQL_NEED_DATA;
		}

		// parameter with bound buffer: 
		// read the data from that buffer and store it as a parameter.

		return SQL_ERROR;
		// TODO: Add the parameter like this (need to implement OdbcTypeConverter::odbcParamToJson):
		//m_currentJsonParameter["value"] = OdbcTypeConverter::getInstance()->odbcParamToJson(m_currentParameterDescriptor);
		//m_jsonParameters.append(m_currentJsonParameter);
		//
		//++m_currentParameter;
	}

	// no more parameters => we can execute the query.
	m_currentParameter = 0;
	Json::Value parameters;

	parameters["Parameters"] = m_jsonParameters;
	parameters["Handle"] = m_handle;

	if (!m_connection->executePostCommand(m_apiResult, "Execute", parameters))
		return SQL_ERROR;


	bool success = m_currentResult.fromJson(m_apiResult);
	return success ? SQL_SUCCESS : SQL_ERROR;
}

ColumnDescriptor* Statement::getColumnDescriptor(uint32_t i)
{
	return m_currentResult.column(i);
}

uint32_t Statement::getNumFetchedRows()
{
	return (uint32_t) m_currentResult.rows().size();
}

void Statement::setQuery(const std::string& query)
{
	m_query = query;
}

bool Statement::bindColumn(uint16_t columnNumber, int16_t targetType, SQLLEN bufferLength, void * targetPointer, SQLLEN * indicatorPointer)
{
	return m_activeRowDescriptor->bind(
		columnNumber,
		targetType,
		bufferLength,
		targetPointer,
		indicatorPointer);
}

bool Statement::bindParameter(uint16_t columnNumber, int16_t targetType, SQLLEN bufferLength, void * targetPointer, SQLLEN * indicatorPointer)
{
	return m_activeParameterDescriptor->bind(
		columnNumber,
		targetType,
		bufferLength,
		targetPointer,
		indicatorPointer);
}


SQLRETURN Statement::getData(SQLUSMALLINT Col_or_Param_Num, SQLSMALLINT TargetType, SQLPOINTER TargetValuePtr, SQLLEN BufferLength, SQLLEN *StrLen_or_IndPtr)
{
	const Json::Value & row = *m_currentResult.rows()[0];
	if (Col_or_Param_Num > row.size()) return SQL_ERROR;

	const Json::Value & value = row[Col_or_Param_Num-1]; // subtract 1 to account for the missing "bookmark column"
	ColumnDescriptor* meta = m_currentResult.column(Col_or_Param_Num);

	if (Helper::isBinaryType(meta->getType()) && Helper::isBinaryType(TargetType))
		return downloadBinaryChunk(Col_or_Param_Num, TargetValuePtr, BufferLength, StrLen_or_IndPtr);

	return OdbcTypeConverter::getInstance()->jsonToOdbc(&value, meta, TargetType, TargetValuePtr, BufferLength, StrLen_or_IndPtr);

}


bool Statement::getTables(std::string catalogName, std::string schemaName, std::string tableName, std::string tableType)
{
	m_cursorPos = 0;

	Json::Value parameters;
	parameters["catalog"] = catalogName;
	parameters["schema"] = schemaName;
	parameters["table"] = tableName;
	parameters["tableType"] = tableType;

	if(!m_connection->executeGetCommand(m_apiResult, "gettables", parameters))
	{
		return false;
	}

	return m_currentResult.fromJson(m_apiResult);
}


SQLRETURN Statement::getColumns(std::string catalogName, std::string schemaName, std::string tableName, std::string columnName)
{
	m_cursorPos = 0;

	Json::Value parameters;
	parameters["catalog"] = catalogName;
	parameters["schema"] = schemaName;
	parameters["table"] = tableName;
	parameters["columnname"] = columnName;

	if (!m_connection->executeGetCommand(m_apiResult, "getcolumns", parameters))
	{
		return false;
	}

	return m_currentResult.fromJson(m_apiResult);
}


SQLRETURN Statement::execute()
{
	m_cursorPos = 0;
	abortBinaryTransfer();

	Json::Value parameters;
	parameters["query"] = m_query;

	
	if (m_handle.size() != 0)
	{
		Json::Value parametersFreeHandle;
		parametersFreeHandle["Handle"] = m_handle;
		m_connection->executePostCommand(m_apiResult, "FreeHandle", parametersFreeHandle);
		m_handle.clear();
	}

	if (!m_connection->executePostCommand(m_apiResult, "PrepareStatement", parameters))
		return SQL_ERROR;
	
	m_handle = m_apiResult["Handle"].asString();
	if (m_handle.size() == 0) return SQL_ERROR;

	// Process parameters - we can go ahead if there are no pending data-at-execution parameters
	return processParametersAndExecute();
}



SQLRETURN Statement::paramData(SQLPOINTER* paramInfo)
{
	if (m_currentParameter == 0) return SQL_ERROR; // nothing to upload
	
	if (!m_isFirstParamUpload)
	{
		// Finish up the previous upload and store its metadata in m_jsonParameters
		// Note: If we were just passed a NULL value in the SQLPutData call, we might
		// not actually have uploaded something (m_isUploadPending == false).
		if (m_isUploadPending)
		{
			if (!m_connection->finishUpload())
			{
				resetParameterUploadState();
				return SQL_ERROR;
			}
		}
		m_jsonParameters.append(m_currentJsonParameter);
		
		// Advance to the next data-on-execute parameter - or execute if this was the last one.
		SQLRETURN result = processParametersAndExecute();
		if (result != SQL_NEED_DATA)
		{
			return result;
		}
	}

	m_isFirstParamUpload = false;

	// Return the paramInfo of the current parameter to the application
	// so that it knows which parameter to upload
	if (paramInfo == NULL) 
		return SQL_ERROR;
	*paramInfo = m_currentParameterDescriptor.getTargetPointer();
	return SQL_NEED_DATA;
}

#define PUTDATA_ASSERT(condition) if(!(condition)) { resetParameterUploadState(); return false; }
bool Statement::putData(SQLPOINTER data, SQLLEN lengthOrIndicator)
{
	// these options are not supported (yet)
	if (lengthOrIndicator == SQL_NTS || lengthOrIndicator == SQL_DEFAULT_PARAM) return false;

	if (!m_isUploadPending)
	{
		// don't upload anything if the data is null
		// we will tell the CDN about the null parameter by passing the null
		// in the metadata instead of the upload handle.
		if (lengthOrIndicator == SQL_NULL_DATA)
		{
			m_currentJsonParameter["value"] = Json::Value(Json::nullValue);
			return true;
		}

		Json::Value jsonParams;
		//jsonParams["Query"] = m_query;
		PUTDATA_ASSERT(m_connection->executeGetCommand(m_apiResult, "RequestHandle", jsonParams));

		try
		{
			m_uploadHandle.clear();
			m_uploadHandle = m_apiResult["Handle"].asString();
			m_currentJsonParameter["value"] = Json::Value(m_uploadHandle);
		}
		catch (Json::Exception)
		{
			PUTDATA_ASSERT(false);
		}

		PUTDATA_ASSERT(m_connection->beginUpload(m_uploadHandle));
		m_isUploadPending = true;
	}

	
	PUTDATA_ASSERT(lengthOrIndicator >= 0); // reject all special values once we are already in upload mode
	PUTDATA_ASSERT(m_connection->uploadChunk(data, lengthOrIndicator));
	return true;
}
#undef PUTDATA_ASSERT


bool Statement::fetchNext()
{
	bool result = fetch(m_cursorPos, 1);
	++m_cursorPos;
	return result;
}

bool Statement::fetch(uint32_t fromRow, uint32_t numRows)
{
	// any download that's currently active becomes invalid when fetching new rows.
	abortBinaryTransfer();

	m_currentResult.rows().clear();
	Json::Value& rows = m_apiResult["Rows"];
	if (rows.isNull()) return false;

	uint32_t rowsAvailable = rows.size() - fromRow;

	// write data to the internal result set
	for (uint32_t i = fromRow; i < fromRow + min(numRows, rowsAvailable); ++i)
	{
		m_currentResult.rows().push_back(&rows[i]);
	}

	// write data to the bound buffers
	if (rowsAvailable > 0)
	{
		m_activeRowDescriptor->populateBoundBuffersFromResultSet(m_currentResult);
	}

	return true;
}