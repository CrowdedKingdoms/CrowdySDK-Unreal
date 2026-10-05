#pragma once

class UCrowdyServerObjectDefinition;

namespace CrowdyExecEditorModule
{
	/** Moves each List whose values are typed by the temporary copy a Blueprint struct recompile makes back onto the struct itself, and dirties the package; true when any List moved. */
	bool RepairDuplicateListTypes(UCrowdyServerObjectDefinition& Definition);
}
