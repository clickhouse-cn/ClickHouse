#include <Storages/NextGenReplication/StorageNextGenReplicatedMergeTree.h>
#include <Storages/NextGenReplication/NextGenReplicatedBlockOutputStream.h>
#include <Storages/MergeTree/ReplicatedMergeTreeAddress.h>
#include <Common/Macros.h>
#include <Common/escapeForFileName.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int NO_ZOOKEEPER;
    extern const int INCORRECT_DATA;
    extern const int REPLICA_IS_ALREADY_EXIST;
}


StorageNextGenReplicatedMergeTree::StorageNextGenReplicatedMergeTree(
    const String & zookeeper_path_,
    const String & replica_name_,
    bool attach,
    const String & path_, const String & database_name_, const String & name_,
    const NamesAndTypesList & columns_,
    const NamesAndTypesList & materialized_columns_,
    const NamesAndTypesList & alias_columns_,
    const ColumnDefaults & column_defaults_,
    Context & context_,
    const ASTPtr & primary_expr_ast_,
    const String & date_column_name,
    const ASTPtr & partition_expr_ast_,
    const ASTPtr & sampling_expression_,
    const MergeTreeData::MergingParams & merging_params_,
    const MergeTreeSettings & settings_,
    bool has_force_restore_data_flag)
    : IStorage(materialized_columns_, alias_columns_, column_defaults_)
    , context(context_)
    , log(&Logger::get(database_name_ + "." + name_ + " (Storage)"))
    , database_name(database_name_), table_name(name_), full_path(path_ + escapeForFileName(table_name) + '/')
    , data(
        database_name, table_name, full_path,
        columns_, materialized_columns_, alias_columns_, column_defaults_,
        context,
        primary_expr_ast_, date_column_name, partition_expr_ast_, sampling_expression_,
        merging_params_, settings_, true, attach,
        [this] (const String & /* part_name */) { /* TODO: enqueue part for check */; })
    , reader(data), writer(data), merger(data, context.getBackgroundPool())
    , parts_fetcher(data)
    , zookeeper_path(context.getMacros().expand(zookeeper_path_))
    , replica_name(context.getMacros().expand(replica_name_))
{
    /// TODO: unify with StorageReplicatedMergeTree and move to common place
    if (!zookeeper_path.empty() && zookeeper_path.back() == '/')
        zookeeper_path.resize(zookeeper_path.size() - 1);
    /// If zookeeper chroot prefix is used, path should starts with '/', because chroot concatenates without it.
    if (!zookeeper_path.empty() && zookeeper_path.front() != '/')
        zookeeper_path = "/" + zookeeper_path;

    replica_path = zookeeper_path + "/replicas/" + replica_name;

    bool skip_sanity_checks = has_force_restore_data_flag; /// TODO: load the flag from ZK.

    data.loadDataParts(skip_sanity_checks);

    auto zookeeper = context.getZooKeeper();

    if (!zookeeper)
    {
        if (!attach)
            throw Exception("Can't create replicated table without ZooKeeper", ErrorCodes::NO_ZOOKEEPER);

        LOG_ERROR(log, "No ZooKeeper: the table will be in readonly mode.");
        is_readonly = true;
        return;
    }

    if (!attach)
    {
        if (!data.getDataParts().empty())
            throw Exception("Data directory for table already containing data parts - probably it was unclean DROP table or manual intervention. You must either clear directory by hand or use ATTACH TABLE instead of CREATE TABLE if you need to use that parts.", ErrorCodes::INCORRECT_DATA);

        createTableOrReplica(*zookeeper);
    }
    else
    {
        /// Temporary directories contain unfinalized results of Merges or Fetches (after forced restart)
        ///  and don't allow to reinitialize them, so delete each of them immediately.
        data.clearOldTemporaryDirectories(0);
    }

    /// TODO: Check table structure.
}

StorageNextGenReplicatedMergeTree::~StorageNextGenReplicatedMergeTree()
{
    try
    {
        shutdown();
    }
    catch(...)
    {
        tryLogCurrentException(__PRETTY_FUNCTION__);
    }
}

void StorageNextGenReplicatedMergeTree::startup()
{
    if (is_readonly)
        return;

    {
        StoragePtr ptr = shared_from_this();
        InterserverIOEndpointPtr endpoint = std::make_shared<DataPartsExchange::Service>(data, ptr);
        parts_exchange_service = std::make_shared<InterserverIOEndpointHolder>(
            endpoint->getId(replica_path), endpoint, context.getInterserverIOHandler());
    }

    {
        auto host_port = context.getInterserverIOAddress();

        /// How other replicas can find us.
        ReplicatedMergeTreeAddress address;
        address.host = host_port.first;
        address.replication_port = host_port.second;
        address.queries_port = context.getTCPPort();
        address.database = database_name;
        address.table = table_name;

        auto zookeeper = getZooKeeper();
        is_active_node = zkutil::EphemeralNodeHolder::create(
            replica_path + "/is_active", *zookeeper, address.toString());

        /// TODO: do something if the node already exists.
    }

    /// TODO initialize the part set from disk.
    part_set_updating_thread = std::thread([this] { runPartSetUpdatingThread(); });

    parts_producing_task = context.getBackgroundPool().addTask([this] { return runPartsProducingTask(); });

    merge_selecting_thread = std::thread([this] { runMergeSelectingThread(); });
}

void StorageNextGenReplicatedMergeTree::createTableOrReplica(zkutil::ZooKeeper & zookeeper)
{
    auto acl = zookeeper.getDefaultACL();

    if (!zookeeper.exists(zookeeper_path))
    {
        LOG_DEBUG(log, "Creating directory structure in ZooKeeper: " << zookeeper_path);

        zookeeper.createAncestors(zookeeper_path);

        /// TODO: save metadata.
        /// TODO: split parts node by partitions.

        zkutil::Ops ops;
        ops.emplace_back(std::make_unique<zkutil::Op::Create>(
            zookeeper_path, String(), acl, zkutil::CreateMode::Persistent));
        ops.emplace_back(std::make_unique<zkutil::Op::Create>(
            zookeeper_path + "/parts", String(), acl, zkutil::CreateMode::Persistent));
        ops.emplace_back(std::make_unique<zkutil::Op::Create>(
            zookeeper_path + "/replicas", String(), acl, zkutil::CreateMode::Persistent));

        auto code = zookeeper.tryMulti(ops);
        if (code != ZOK && code != ZNODEEXISTS)
            throw zkutil::KeeperException(code);
    }

    LOG_DEBUG(log, "Creating replica " << replica_path);
    zkutil::Ops ops;
    ops.emplace_back(std::make_unique<zkutil::Op::Create>(
        replica_path, String(), acl, zkutil::CreateMode::Persistent));

    try
    {
        zookeeper.multi(ops);
    }
    catch (const zkutil::KeeperException & e)
    {
        if (e.code == ZNODEEXISTS)
            throw Exception("Replica " + replica_path + " already exists.", ErrorCodes::REPLICA_IS_ALREADY_EXIST);

        throw;
    }
}

void StorageNextGenReplicatedMergeTree::shutdown()
{
    shutdown_called = true;

    if (merge_selecting_thread.joinable())
    {
        /// TODO: needs protection from concurrent drops.
        merge_selecting_event->set();
        merge_selecting_thread.join();
    }

    parts_fetcher.blocker.cancelForever();

    merger.merges_blocker.cancelForever();

    if (parts_producing_task)
    {
        context.getBackgroundPool().removeTask(parts_producing_task);
        parts_producing_task.reset();
    }

    if (part_set_updating_thread.joinable())
    {
        /// TODO: needs protection from concurrent drops.
        part_set_updating_event->set();
        part_set_updating_thread.join();
    }

    is_active_node.reset();

    if (parts_exchange_service)
    {
        parts_exchange_service->cancelForever();
        parts_exchange_service.reset();
    }
}

BlockInputStreams StorageNextGenReplicatedMergeTree::read(
    const Names & column_names,
    const SelectQueryInfo & query_info,
    const Context & context,
    QueryProcessingStage::Enum & processed_stage,
    size_t max_block_size,
    unsigned num_streams)
{
    /// TODO: quorum reads

    return reader.read(column_names, query_info, context, processed_stage, max_block_size, num_streams, 0);
}

BlockOutputStreamPtr StorageNextGenReplicatedMergeTree::write(const ASTPtr & /*query*/, const Settings &)
{
    /// TODO: deduplication

    return std::make_shared<NextGenReplicatedBlockOutputStream>(*this);
}

void StorageNextGenReplicatedMergeTree::drop()
{
    {
        auto zookeeper = tryGetZooKeeper();
        if (is_readonly || !zookeeper || zookeeper->expired())
            throw Exception("Can't drop readonly replicated table (need to drop data in ZooKeeper as well)", ErrorCodes::NO_ZOOKEEPER);

        shutdown();

        LOG_INFO(log, "Removing replica " << replica_path << " from ZooKeeper");

        zookeeper->tryRemoveRecursive(replica_path);

        Strings replicas;
        if (zookeeper->tryGetChildren(zookeeper_path + "/replicas", replicas) == ZOK && replicas.empty())
        {
            LOG_INFO(log, "Removing table " << zookeeper_path << " from ZooKeeper");
            zookeeper->tryRemoveRecursive(zookeeper_path);
        }
    }

    data.dropAllData();
}

zkutil::ZooKeeperPtr StorageNextGenReplicatedMergeTree::tryGetZooKeeper()
{
    /// TODO: async reconnect?

    return context.tryGetZooKeeper();
}

zkutil::ZooKeeperPtr StorageNextGenReplicatedMergeTree::getZooKeeper()
{
    auto res = tryGetZooKeeper();
    if (!res)
        throw Exception("Cannot get ZooKeeper", ErrorCodes::NO_ZOOKEEPER);
    if (res->expired())
        throw Exception("ZooKeeper session expired", ErrorCodes::NO_ZOOKEEPER);
    return res;
}

void StorageNextGenReplicatedMergeTree::runPartSetUpdatingThread()
{
    while (!shutdown_called)
    {
        try
        {
            auto zookeeper = getZooKeeper();
            Strings part_nodes = zookeeper->getChildren(zookeeper_path + "/parts", nullptr, part_set_updating_event);

            bool have_new_parts = false;
            {
                std::lock_guard<std::mutex> lock(parts_mutex);
                /// TODO maybe not the best idea to lock the mutex for so long.

                for (auto it = parts.begin(); it != parts.end(); )
                {
                    if (it->second.state == Part::State::Ephemeral)
                        it = parts.erase(it);
                    else
                        ++it;
                }

                for (const String & part_name : part_nodes)
                {
                    if (startsWith(part_name, "insert_"))
                    {
                        /// TODO extract ephemeral part name creation/parsing code.
                        const char * begin = part_name.data() + strlen("insert_");
                        const char * end = part_name.data() + part_name.length();

                        String partition_id;
                        for (; begin != end && *begin != '_'; ++begin)
                            partition_id.push_back(*begin);

                        if (begin == end)
                            throw Exception("Bad ephemeral insert node name: " + part_name, ErrorCodes::LOGICAL_ERROR);

                        ++begin;
                        UInt64 block_num = parse<UInt64>(begin, end - begin);

                        MergeTreePartInfo part_info(partition_id, block_num, block_num, 0);
                        parts.emplace(part_info, Part{part_info, Part::State::Ephemeral});
                    }
                    else
                    {
                        auto part_info = MergeTreePartInfo::fromPartName(part_name, data.format_version);
                        auto insertion = parts.emplace(part_info, Part{part_info, Part::State::Virtual});
                        if (insertion.second)
                        {
                            LOG_TRACE(log, "New part: " << part_name);
                            have_new_parts = true;
                        }
                    }
                }
            }

            if (have_new_parts)
                parts_producing_task->wake();
        }
        catch (...)
        {
            tryLogCurrentException(__PRETTY_FUNCTION__);
            part_set_updating_event->tryWait(1000);
        }

        part_set_updating_event->wait();
    }
}

bool StorageNextGenReplicatedMergeTree::runPartsProducingTask()
{
    return false;
}

void StorageNextGenReplicatedMergeTree::runMergeSelectingThread()
{
    while (!shutdown_called)
    {
        merge_selecting_event->tryWait(1000);
    }
}

}