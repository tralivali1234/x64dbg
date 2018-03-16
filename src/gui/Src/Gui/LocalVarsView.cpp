#include "LocalVarsView.h"
#include "zydis_wrapper.h"
#include "CPUMultiDump.h"
#include "MiscUtil.h"
#include "WordEditDialog.h"
#include "EncodeMap.h"

// Gets the address from an expression like "[EBP-7c]"
static bool getAddress(QString addrText, duint & output)
{
    if(!(addrText.startsWith("[") && addrText.endsWith("]")))
        return false; //Unable to get address of the expression
    addrText.truncate(addrText.size() - 1);
    addrText = addrText.right(addrText.size() - 1);
    QByteArray utf8 = addrText.toUtf8();
    if(!DbgIsValidExpression(utf8.constData()))
        return false;
    output = DbgValFromString(utf8.constData());
    return true;
}

LocalVarsView::LocalVarsView(CPUMultiDump* parent) : StdTable(parent)
{
    configUpdatedSlot();

    int charWidth = getCharWidth();
    addColumnAt(8 + 20 * charWidth, tr("Name"), true);
    addColumnAt(8 + 10 * charWidth, tr("Expression"), true); //[EBP + 10]
    addColumnAt(8 + 10 * charWidth, tr("Value"), true);
    loadColumnFromConfig("LocalVarsView");

    setupContextMenu();
    connect(Bridge::getBridge(), SIGNAL(updateWatch()), this, SLOT(updateSlot()));
    connect(Config(), SIGNAL(tokenizerConfigUpdated()), this, SLOT(configUpdatedSlot()));
    connect(this, SIGNAL(doubleClickedSignal()), this, SLOT(editSlot()));
}

void LocalVarsView::setupContextMenu()
{
    mMenu = new MenuBuilder(this, [](QMenu*)
    {
        return DbgIsDebugging();
    });
    mMenu->addAction(makeAction(DIcon("dump.png"), tr("&Follow in Dump"), SLOT(followDumpSlot())), [this](QMenu*)
    {
        return getCellContent(getInitialSelection(), 2) != "???";
    });
    mMenu->addAction(makeAction(DIcon("dump.png"), ArchValue(tr("Follow DWORD in Dump"), tr("Follow QWORD in Dump")), SLOT(followWordInDumpSlot())), [this](QMenu*)
    {
        duint start;
        if(getAddress(getCellContent(getInitialSelection(), 1), start))
        {
            DbgMemRead(start, &start, sizeof(start));
            return DbgMemIsValidReadPtr(start);
        }
        else
            return false;
    });
    mMenu->addAction(makeShortcutAction(DIcon("stack.png"), tr("Follow in Stack"), SLOT(followStackSlot()), "ActionFollowStack"), [this](QMenu*)
    {
        duint start;
        if(getAddress(getCellContent(getInitialSelection(), 1), start))
            return (DbgMemIsValidReadPtr(start) && DbgMemFindBaseAddr(start, 0) == DbgMemFindBaseAddr(DbgValFromString("csp"), 0));
        else
            return false;
    });
    mMenu->addAction(makeAction(DIcon("stack.png"), ArchValue(tr("Follow DWORD in Stack"), tr("Follow QWORD in Stack")), SLOT(followWordInStackSlot())), [this](QMenu*)
    {
        duint start;
        if(getAddress(getCellContent(getInitialSelection(), 1), start))
        {
            DbgMemRead(start, &start, sizeof(start));
            return (DbgMemIsValidReadPtr(start) && DbgMemFindBaseAddr(start, 0) == DbgMemFindBaseAddr(DbgValFromString("csp"), 0));
        }
        else
            return false;
    });
    mMenu->addAction(makeShortcutAction(DIcon("memmap_find_address_page.png"), tr("Follow in Memory Map"), SLOT(followMemMapSlot()), "ActionFollowMemMap"), [this](QMenu*)
    {
        return getCellContent(getInitialSelection(), 2) != "???";
    });
    mMenu->addAction(makeShortcutAction(DIcon("modify.png"), tr("&Modify Value"), SLOT(editSlot()), "ActionModifyValue"), [this](QMenu*)
    {
        return getCellContent(getInitialSelection(), 2) != "???";
    });
    mMenu->addSeparator();
    mMenu->addAction(makeAction(tr("&Rename"), SLOT(renameSlot())));
    MenuBuilder* copyMenu = new MenuBuilder(this);
    setupCopyMenu(copyMenu);
    mMenu->addMenu(makeMenu(DIcon("copy.png"), tr("&Copy")), copyMenu);
    mMenu->addSeparator();
    MenuBuilder* mBaseRegisters = new MenuBuilder(this);
#ifdef _WIN64
    baseRegisters[0] = new QAction("RAX", this);
    baseRegisters[1] = new QAction("RBX", this);
    baseRegisters[2] = new QAction("RCX", this);
    baseRegisters[3] = new QAction("RDX", this);
    baseRegisters[4] = new QAction("RBP", this);
    baseRegisters[5] = new QAction("RSP", this);
    baseRegisters[6] = new QAction("RSI", this);
    baseRegisters[7] = new QAction("RDI", this);
    baseRegisters[8] = new QAction("R8", this);
    baseRegisters[9] = new QAction("R9", this);
    baseRegisters[10] = new QAction("R10", this);
    baseRegisters[11] = new QAction("R11", this);
    baseRegisters[12] = new QAction("R12", this);
    baseRegisters[13] = new QAction("R13", this);
    baseRegisters[14] = new QAction("R14", this);
    baseRegisters[15] = new QAction("R15", this);
    for(char i = 0; i < 16; i++)
    {
        connect(baseRegisters[i], SIGNAL(triggered()), this, SLOT(baseChangedSlot()));
        baseRegisters[i]->setCheckable(true);
        mBaseRegisters->addAction(baseRegisters[i]);
    }
#else //x86
    baseRegisters[0] = new QAction("EAX", this);
    baseRegisters[1] = new QAction("EBX", this);
    baseRegisters[2] = new QAction("ECX", this);
    baseRegisters[3] = new QAction("EDX", this);
    baseRegisters[4] = new QAction("EBP", this);
    baseRegisters[5] = new QAction("ESP", this);
    baseRegisters[6] = new QAction("ESI", this);
    baseRegisters[7] = new QAction("EDI", this);
    for(char i = 0; i < 8; i++)
    {
        connect(baseRegisters[i], SIGNAL(triggered()), this, SLOT(baseChangedSlot()));
        baseRegisters[i]->setCheckable(true);
        mBaseRegisters->addAction(baseRegisters[i]);
    }
#endif //_WIN64
    baseRegisters[4]->setChecked(true); //CBP
    mMenu->addMenu(makeMenu(tr("Base Register")), mBaseRegisters);
    connect(this, SIGNAL(contextMenuSignal(QPoint)), this, SLOT(contextMenuSlot(QPoint)));
}

void LocalVarsView::mousePressEvent(QMouseEvent* event)
{
    if(event->buttons() == Qt::MiddleButton && getRowCount() > 0)
    {
        duint addr;
        if(getAddress(getCellContent(getInitialSelection(), 1), addr))
            Bridge::CopyToClipboard(ToPtrString(addr));
    }
    else
        StdTable::mousePressEvent(event);
}

void LocalVarsView::contextMenuSlot(const QPoint & pos)
{
    QMenu wMenu(this);
    mMenu->build(&wMenu);
    wMenu.exec(mapToGlobal(pos));
}

void LocalVarsView::baseChangedSlot()
{
    currentFunc = 0;
    updateSlot();
}

void LocalVarsView::configUpdatedSlot()
{
    currentFunc = 0;
    HexPrefixValues = Config()->getBool("Disassembler", "0xPrefixValues");
    MemorySpaces = Config()->getBool("Disassembler", "MemorySpaces");
}

void LocalVarsView::updateSlot()
{
    if(!DbgIsDebugging())
    {
        currentFunc = 0;
        setRowCount(0);
        reloadData();
        return;
    }
    REGDUMP z;
    DbgGetRegDumpEx(&z, sizeof(REGDUMP));
    duint start, end;

    if(DbgFunctionGet(z.regcontext.cip, &start, &end))
    {
        if(start != this->currentFunc) //needs analyzing
        {
            Zydis dis;
            unsigned char* buffer = new unsigned char[end - start + 16];
            if(!DbgMemRead(start, buffer, end - start + 16)) //failed to read memory for analyzing
            {
                delete[] buffer;
                setRowCount(0);
                return;
            }
            QSet<dsint> usedOffsets[ArchValue(8, 16)];
            duint address = start;
            while(address < end)
            {
                ENCODETYPE codeType = DbgGetEncodeTypeAt(address, 1);
                if(!EncodeMap::isCode(codeType)) //Skip data bytes
                {
                    address += DbgGetEncodeSizeAt(address, 1);
                    continue;
                }
                dis.Disassemble(address, buffer + address - start, end + 16 - address);
                if(dis.IsNop()) //Skip junk instructions
                {
                    address += dis.Size();
                    continue;
                }
                for(int i = 0; i < dis.OpCount(); i++)
                {
                    if(dis[i].type != ZYDIS_OPERAND_TYPE_MEMORY)
                        continue;
                    if(dis[i].mem.base == ZYDIS_REGISTER_NONE) //mov eax, [10000000], global variable
                        continue;
                    if(dis[i].mem.index != ZYDIS_REGISTER_NONE) //mov eax, dword ptr ds:[ebp+ecx*4-10], indexed array
                        continue;
                    const ZydisRegister registers[] =
                    {
#ifdef _WIN64
                        ZYDIS_REGISTER_RAX, ZYDIS_REGISTER_RBX, ZYDIS_REGISTER_RCX, ZYDIS_REGISTER_RDX,
                        ZYDIS_REGISTER_RBP, ZYDIS_REGISTER_RSP, ZYDIS_REGISTER_RSI, ZYDIS_REGISTER_RDI,
                        ZYDIS_REGISTER_R8, ZYDIS_REGISTER_R9, ZYDIS_REGISTER_R10, ZYDIS_REGISTER_R11,
                        ZYDIS_REGISTER_R12, ZYDIS_REGISTER_R13, ZYDIS_REGISTER_R14, ZYDIS_REGISTER_R15
#else //x86
                        ZYDIS_REGISTER_EAX, ZYDIS_REGISTER_EBX, ZYDIS_REGISTER_ECX, ZYDIS_REGISTER_EDX,
                        ZYDIS_REGISTER_EBP, ZYDIS_REGISTER_ESP, ZYDIS_REGISTER_ESI, ZYDIS_REGISTER_EDI
#endif //_WIN64
                    };
                    for(char j = 0; j < ArchValue(8, 16); j++)
                    {
                        if(!baseRegisters[j]->isChecked())
                            continue;
                        if(dis[i].mem.base == registers[j])
                            usedOffsets[j].insert(dis[i].mem.disp.value);
                    }
                }
                address += dis.Size();
            }
            delete[] buffer;
            int rows = 0;
            for(int i = 0; i < ArchValue(8, 16); i++)
                rows += usedOffsets[i].size();
            setRowCount(rows);
            rows = 0;
            for(int i = 0; i < ArchValue(8, 16); i++)
            {
                std::vector<dsint> sorted;
                sorted.reserve(usedOffsets[i].size());
                for(const auto & j : usedOffsets[i])
                    sorted.push_back(j);
                std::sort(sorted.begin(), sorted.end(), std::greater<dsint>());
                for(const auto & j : sorted)
                {
                    QString expr;
                    QString name;
                    if(j < 0)
                    {
                        expr = QString("%1").arg(-j, 0, 16);
                        if(HexPrefixValues)
                            expr = "0x" + expr;
                        if(!MemorySpaces)
                            expr = "-" + expr;
                        else
                            expr = " - " + expr;
                    }
                    else
                    {
                        expr = QString("%1").arg(j, 0, 16);
                        if(HexPrefixValues)
                            expr = "0x" + expr;
                        if(!MemorySpaces)
                            expr = "+" + expr;
                        else
                            expr = " + " + expr;
                    }
                    expr = QString("[%1%2]").arg(baseRegisters[i]->text()).arg(expr);
                    if(i == 4) //CBP
                    {
                        if(j < 0)
                            name = tr("Local%1").arg(-j / sizeof(dsint)); //EBP-C:Local3
                        else
                            name = tr("Arg%1").arg(j / sizeof(dsint) - 1); //EBP+C:Arg2
                    }
                    else
                    {
                        if(j < 0)
                            name = QString("_%2_%1").arg(-j, 0, 16); //ECX-C: _ECX_C
                        else
                            name = QString("%2_%1").arg(j, 0, 16); //ECX+C: ECX_C
                        name = name.arg(baseRegisters[i]->text());
                    }
                    setCellContent(rows, 0, name);
                    setCellContent(rows, 1, expr);
                    rows++;
                }
            } // Analyze finish
            this->currentFunc = start;
        }
        for(dsint i = 0; i < getRowCount(); i++)
        {
            duint val = 0;
            QByteArray buf = getCellContent(i, 1).toUtf8();
            if(DbgIsValidExpression(buf.constData()))
            {
                val = DbgValFromString(buf.constData());
                setCellContent(i, 2, getSymbolicNameStr(val));
            }
            else
                setCellContent(i, 2, "???");
        }
        reloadData();
    }
    else
    {
        currentFunc = 0;
        setRowCount(0);
        reloadData();
    }
}

void LocalVarsView::renameSlot()
{
    QString newName;
    if(getRowCount() > 0)
    {
        QString oldName = getCellContent(getInitialSelection(), 0);
        if(SimpleInputBox(this, tr("Rename local variable \"%1\"").arg(oldName), oldName, newName, oldName))
        {
            setCellContent(getInitialSelection(), 0, newName);
            reloadData();
        }
    }
}

void LocalVarsView::editSlot()
{
    if(getRowCount() <= 0)
        return;
    QString expr = getCellContent(getInitialSelection(), 1);
    duint addr;
    if(!getAddress(expr, addr))
        return;
    WordEditDialog editor(this);
    duint current = 0;
    if(!DbgMemRead(addr, &current, sizeof(duint)))
        return; //Memory is not accessible
    editor.setup(tr("Edit %1 at %2").arg(getCellContent(getInitialSelection(), 0)).arg(ToPtrString(addr)), current, sizeof(dsint));
    if(editor.exec() == QDialog::Accepted)
    {
        current = editor.getVal();
        DbgMemWrite(addr, &current, sizeof(duint));
        emit updateSlot();
    }
}

void LocalVarsView::followDumpSlot()
{
    duint addr;
    if(getAddress(getCellContent(getInitialSelection(), 1), addr))
        DbgCmdExec(QString("dump %1").arg(ToPtrString(addr)).toUtf8().constData());
}

void LocalVarsView::followStackSlot()
{
    duint addr;
    if(getAddress(getCellContent(getInitialSelection(), 1), addr))
        DbgCmdExec(QString("sdump %1").arg(ToPtrString(addr)).toUtf8().constData());
}

void LocalVarsView::followMemMapSlot()
{
    duint addr;
    if(getAddress(getCellContent(getInitialSelection(), 1), addr))
        DbgCmdExec(QString("memmapdump %1").arg(ToPtrString(addr)).toUtf8().constData());
}

void LocalVarsView::followWordInDumpSlot()
{
    duint addr;
    if(getAddress(getCellContent(getInitialSelection(), 1), addr))
        DbgCmdExec(QString("dump [%1]").arg(ToPtrString(addr)).toUtf8().constData());
}

void LocalVarsView::followWordInStackSlot()
{
    duint addr;
    if(getAddress(getCellContent(getInitialSelection(), 1), addr))
        DbgCmdExec(QString("sdump [%1]").arg(ToPtrString(addr)).toUtf8().constData());
}
