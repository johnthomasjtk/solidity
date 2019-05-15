/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
/**
 * Optimisation stage that removes unused variables and functions.
 */

#include <libyul/optimiser/ConstantOptimiser.h>

#include <libyul/optimiser/ASTCopier.h>
#include <libyul/optimiser/Metrics.h>
#include <libyul/AsmData.h>
#include <libyul/AsmPrinter.h>
#include <libyul/Utilities.h>
#include <libyul/AsmParser.h>

#include <libdevcore/CommonData.h>

using namespace std;
using namespace dev;
using namespace yul;

using Representation = ConstantOptimiser::Representation;

void ConstantOptimiser::visit(Expression& _e)
{
	if (_e.type() == typeid(Literal))
	{
		Literal const& literal = boost::get<Literal>(_e);
		if (literal.kind != LiteralKind::Number)
			return;
		u256 value = valueOfLiteral(literal);
		if (value < 0x10000)
			return;

		if (
			Expression const* repr =
			RepresentationFinder(m_evmVersion, m_meter, locationOf(_e), m_cache)
			.tryFindRepresentation(value)
		)
			_e = ASTCopier{}.translate(*repr);
	}
	else
		ASTModifier::visit(_e);
}

Expression const* RepresentationFinder::tryFindRepresentation(dev::u256 const& _value)
{
	if (_value < 0x10000)
		return nullptr;

	Representation repr = findRepresentation(_value);
	if (repr.expression->type() == typeid(Literal))
		return nullptr;
	else
		return repr.expression.get();
}

Representation RepresentationFinder::findRepresentation(dev::u256 const& _value)
{
	if (m_cache.count(_value))
		return m_cache.at(_value);

	Representation routine;
	if (_value <= 0x10000)
		routine = min(move(routine), represent(_value));
	else if (dev::bytesRequired(~_value) < dev::bytesRequired(_value))
		// Negated is shorter to represent
		routine = min(move(routine), represent("not", findRepresentation(~_value)));
	else
	{
		// Decompose value into a * 2**k + b where abs(b) << 2**k
		// Is not always better, try literal and decomposition method.
		routine = represent(_value);

		for (unsigned bits = 255; bits > 8 && m_maxSteps > 0; --bits)
		{
			unsigned gapDetector = unsigned((_value >> (bits - 8)) & 0x1ff);
			if (gapDetector != 0xff && gapDetector != 0x100)
				continue;

			u256 powerOfTwo = u256(1) << bits;
			u256 upperPart = _value >> bits;
			bigint lowerPart = _value & (powerOfTwo - 1);
			if ((powerOfTwo - lowerPart) < lowerPart)
			{
				lowerPart = lowerPart - powerOfTwo; // make it negative
				upperPart++;
			}
			if (upperPart == 0)
				continue;
			if (abs(lowerPart) >= (powerOfTwo >> 8))
				continue;
			Representation newRoutine;
			if (m_evmVersion.hasBitwiseShifting())
				newRoutine = represent("shl", represent(bits), findRepresentation(upperPart));
			else
			{
				newRoutine = represent("exp", represent(2), represent(bits));
				if (upperPart != 1)
					newRoutine = represent("mul", findRepresentation(upperPart), newRoutine);
			}

			if (newRoutine.cost >= routine.cost)
				continue;

			if (lowerPart > 0)
				newRoutine = represent("add", newRoutine, findRepresentation(u256(abs(lowerPart))));
			else if (lowerPart < 0)
				newRoutine = represent("sub", newRoutine, findRepresentation(u256(abs(lowerPart))));

			if (m_maxSteps > 0)
				m_maxSteps--;
			routine = min(move(routine), move(newRoutine));
		}
	}
	m_cache[_value] = routine;
	return routine;
}

Representation RepresentationFinder::represent(dev::u256 const& _value) const
{
	Representation repr;
	repr.expression = make_shared<Expression>(Literal{m_location, LiteralKind::Number, YulString{formatNumber(_value)}, {}});
	repr.cost = m_meter.costs(*repr.expression);
	return repr;
}

Representation RepresentationFinder::represent(
	string const& _instruction,
	Representation const& _argument
) const
{
	dev::eth::Instruction instr = Parser::instructions().at(_instruction);
	Representation repr;
	repr.expression = make_shared<Expression>(FunctionalInstruction{
		m_location,
		instr,
		{ASTCopier{}.translate(*_argument.expression)}
	});
	repr.cost = _argument.cost + m_meter.instructionCosts(instr);
	return repr;
}

Representation RepresentationFinder::represent(
	string const& _instruction,
	Representation const& _arg1,
	Representation const& _arg2
) const
{
	dev::eth::Instruction instr = Parser::instructions().at(_instruction);
	Representation repr;
	repr.expression = make_shared<Expression>(FunctionalInstruction{
		m_location,
		instr,
		{ASTCopier{}.translate(*_arg1.expression), ASTCopier{}.translate(*_arg2.expression)}
	});
	repr.cost = m_meter.instructionCosts(instr) + _arg1.cost + _arg2.cost;
	return repr;
}

Representation RepresentationFinder::min(Representation _a, Representation _b)
{
	if (_a.cost <= _b.cost)
		return _a;
	else
		return _b;
}
