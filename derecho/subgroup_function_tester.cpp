/**
 * @file subgroup_function_tester.cpp
 *
 * @date May 24, 2017
 * @author edward
 */

#include <iostream>
#include <vector>

#include "subgroup_function_tester.h"

std::string ip_generator() {
    static int invocation_count = 0;
    std::stringstream string_generator;
    string_generator << "192.168.1." << invocation_count;
    ++invocation_count;
    return string_generator.str();
}

struct TestType1 {};
struct TestType2 {};
struct TestType3 {};
struct TestType4 {};
struct TestType5 {};
struct TestType6 {};

int main(int argc, char* argv[]) {
    using derecho::SubgroupAllocationPolicy;
    using derecho::CrossProductPolicy;
    using derecho::DefaultSubgroupAllocator;
    using derecho::CrossProductAllocator;

    //Reduce the verbosity of specifying "ordered" for three custom subgroups
    std::vector<derecho::Mode> three_ordered(3, derecho::Mode::ORDERED);
    SubgroupAllocationPolicy sharded_policy = derecho::one_subgroup_policy(derecho::even_sharding_policy(5, 3));
    SubgroupAllocationPolicy unsharded_policy = derecho::one_subgroup_policy(derecho::even_sharding_policy(1, 5));
    SubgroupAllocationPolicy uneven_sharded_policy = derecho::one_subgroup_policy(
            derecho::custom_shards_policy({2, 5, 3}, three_ordered));
    SubgroupAllocationPolicy multiple_copies_policy = derecho::identical_subgroups_policy(
            2, derecho::even_sharding_policy(3, 4));
    SubgroupAllocationPolicy multiple_subgroups_policy{3, false, {derecho::even_sharding_policy(3, 3), derecho::custom_shards_policy({4, 3, 4}, three_ordered), derecho::even_sharding_policy(2, 2)}};

    //This will create subgroups that are the cross product of the "uneven_sharded_policy" and "sharded_policy" groups
    CrossProductPolicy uneven_to_even_cp{
        {std::type_index(typeid(TestType3)), 0},
        {std::type_index(typeid(TestType1)), 0}
    };

    //We're really just testing the allocation functions, so assign each one to a dummy Replicated type
    derecho::SubgroupInfo test_subgroups{
        { {std::type_index(typeid(TestType1)), DefaultSubgroupAllocator(sharded_policy)},
          {std::type_index(typeid(TestType2)), DefaultSubgroupAllocator(unsharded_policy)},
          {std::type_index(typeid(TestType3)), DefaultSubgroupAllocator(uneven_sharded_policy)},
          {std::type_index(typeid(TestType4)), DefaultSubgroupAllocator(multiple_copies_policy)},
          {std::type_index(typeid(TestType5)), DefaultSubgroupAllocator(multiple_subgroups_policy)},
          {std::type_index(typeid(TestType6)), CrossProductAllocator(uneven_to_even_cp)}
        },
        { std::type_index(typeid(TestType1)), std::type_index(typeid(TestType2)), std::type_index(typeid(TestType3)),
        std::type_index(typeid(TestType4)), std::type_index(typeid(TestType5)), std::type_index(typeid(TestType6)) }
    };

    std::vector<derecho::node_id_t> members(100);
    std::iota(members.begin(), members.end(), 0);
    std::vector<derecho::ip_addr> member_ips(100);
    std::generate(member_ips.begin(), member_ips.end(), ip_generator);
    std::vector<char> none_failed(100, 0);
    auto curr_view = std::make_unique<derecho::View>(0, members, member_ips, none_failed);

    std::cout << "TEST 1: Initial allocation" << std::endl;
    derecho::test_provision_subgroups(test_subgroups, nullptr, *curr_view);

    std::set<int> ranks_to_fail{1, 3, 17, 38, 40};
    std::cout << "TEST 2: Failing some nodes that are in subgroups: " << ranks_to_fail << std::endl;
    std::unique_ptr<derecho::View> prev_view(std::move(curr_view));
    curr_view = derecho::make_next_view(*prev_view, ranks_to_fail, {}, {});

    derecho::test_provision_subgroups(test_subgroups, prev_view, *curr_view);

    std::set<int> more_ranks_to_fail{13, 20, 59, 78, 89};
    std::cout << "TEST 3: Failing nodes both before and after the pointer. Ranks are " << more_ranks_to_fail << std::endl;
    prev_view.swap(curr_view);
    curr_view = derecho::make_next_view(*prev_view, more_ranks_to_fail, {}, {});

    derecho::test_provision_subgroups(test_subgroups, prev_view, *curr_view);

    //There are now 90 members left, so fail ranks 39-89
    std::vector<int> range_39_to_89(50);
    std::iota(range_39_to_89.begin(), range_39_to_89.end(), 39);
    std::set<int> lots_of_members_to_fail(range_39_to_89.begin(), range_39_to_89.end());
    std::cout << "TEST 4: Failing 50 nodes so the next view is inadequate" << std::endl;
    prev_view.swap(curr_view);
    curr_view = derecho::make_next_view(*prev_view, lots_of_members_to_fail, {}, {});

    derecho::test_provision_subgroups(test_subgroups, prev_view, *curr_view);

    std::vector<derecho::node_id_t> new_members(40);
    std::iota(new_members.begin(), new_members.end(), 100);
    std::vector<derecho::ip_addr> new_member_ips(40);
    std::generate(new_member_ips.begin(), new_member_ips.end(), ip_generator);
    std::cout << "TEST 5: Adding new members 100-140" << std::endl;
    prev_view.swap(curr_view);
    curr_view = derecho::make_next_view(*prev_view, {}, new_members, new_member_ips);

    derecho::test_provision_subgroups(test_subgroups, prev_view, *curr_view);

    return 0;
}

namespace derecho {

void print_subgroup_layout(const subgroup_shard_layout_t& layout) {
    using std::cout;
    for(std::size_t subgroup_num = 0; subgroup_num < layout.size(); ++subgroup_num) {
        cout << "Subgroup " << subgroup_num << ": ";
        for(std::size_t shard_num = 0; shard_num < layout[subgroup_num].size(); ++shard_num) {
            cout << layout[subgroup_num][shard_num].members << ", ";
        }
        cout << "\b\b" << std::endl;
    }
}

void test_provision_subgroups(const SubgroupInfo& subgroup_info,
                              const std::unique_ptr<View>& prev_view,
                              View& curr_view) {
    bool previous_was_ok = !prev_view || prev_view->is_adequately_provisioned;
    int32_t initial_next_unassigned_rank = curr_view.next_unassigned_rank;
    std::cout << "View has these members: " << curr_view.members << std::endl;
    for(const auto& subgroup_type : subgroup_info.membership_function_order) {
        subgroup_shard_layout_t subgroup_shard_views;
        try {
            auto temp = subgroup_info.subgroup_membership_functions.at(subgroup_type)(curr_view, curr_view.next_unassigned_rank, previous_was_ok);
            subgroup_shard_views = std::move(temp);
            std::cout << "Subgroup type " << subgroup_type.name() << " got assignment: " << std::endl;
            derecho::print_subgroup_layout(subgroup_shard_views);
            std::cout << "next_unassigned_rank is " << curr_view.next_unassigned_rank << std::endl
                      << std::endl;
        } catch(derecho::subgroup_provisioning_exception& ex) {
            curr_view.is_adequately_provisioned = false;
            curr_view.next_unassigned_rank = initial_next_unassigned_rank;
            curr_view.subgroup_shard_views.clear();
            std::cout << "Subgroup type " << subgroup_type.name() << " failed to provision, marking View inadequate" << std::endl
                      << std::endl;
            return;
        }
        std::size_t num_subgroups = subgroup_shard_views.size();
        curr_view.subgroup_ids_by_type[subgroup_type] = std::vector<subgroup_id_t>(num_subgroups);
        for(uint32_t subgroup_index = 0; subgroup_index < num_subgroups; ++subgroup_index) {
            //Assign this (type, index) pair a new unique subgroup ID
            subgroup_id_t next_subgroup_number = curr_view.subgroup_shard_views.size();
            curr_view.subgroup_ids_by_type[subgroup_type][subgroup_index] = next_subgroup_number;
            uint32_t num_shards = subgroup_shard_views.at(subgroup_index).size();
            for(uint shard_num = 0; shard_num < num_shards; ++shard_num) {
                SubView& shard_view = subgroup_shard_views.at(subgroup_index).at(shard_num);
                //Initialize my_rank in the SubView for this node's ID
                shard_view.my_rank = shard_view.rank_of(curr_view.members[curr_view.my_rank]);
                if(prev_view && prev_view->is_adequately_provisioned) {
                    //Initialize this shard's SubView.joined and SubView.departed
                    subgroup_id_t prev_subgroup_id = prev_view->subgroup_ids_by_type
                                                             .at(subgroup_type)
                                                             .at(subgroup_index);
                    SubView& prev_shard_view = prev_view->subgroup_shard_views[prev_subgroup_id][shard_num];
                    std::set<node_id_t> prev_members(prev_shard_view.members.begin(), prev_shard_view.members.end());
                    std::set<node_id_t> curr_members(shard_view.members.begin(), shard_view.members.end());
                    std::set_difference(curr_members.begin(), curr_members.end(),
                                        prev_members.begin(), prev_members.end(),
                                        std::back_inserter(shard_view.joined));
                    std::set_difference(prev_members.begin(), prev_members.end(),
                                        curr_members.begin(), curr_members.end(),
                                        std::back_inserter(shard_view.departed));
                }
            }
            /* Pull the shard->SubView mapping out of the subgroup membership list
             * and save it under its subgroup ID (which was shard_views_by_subgroup.size()) */
            curr_view.subgroup_shard_views.emplace_back(
                    std::move(subgroup_shard_views[subgroup_index]));
        }

    }
}

std::unique_ptr<View> make_next_view(const View& curr_view,
                                     const std::set<int>& leave_ranks,
                                     const std::vector<node_id_t>& joiner_ids,
                                     const std::vector<ip_addr>& joiner_ips) {
    int next_num_members = curr_view.num_members - leave_ranks.size() + joiner_ids.size();
    std::vector<node_id_t> joined, members(next_num_members), departed;
    std::vector<char> failed(next_num_members);
    std::vector<ip_addr> member_ips(next_num_members);
    int next_unassigned_rank = curr_view.next_unassigned_rank;
    for(std::size_t i = 0; i < joiner_ids.size(); ++i) {
        joined.emplace_back(joiner_ids[i]);
        //New members go at the end of the members list, but it may shrink in the new view
        int new_member_rank = curr_view.num_members - leave_ranks.size() + i;
        members[new_member_rank] = joiner_ids[i];
        member_ips[new_member_rank] = joiner_ips[i];
    }
    for(const auto& leaver_rank : leave_ranks) {
        departed.emplace_back(curr_view.members[leaver_rank]);
        //Decrement next_unassigned_rank for every failure, unless the failure wasn't in a subgroup anyway
        if(leaver_rank <= curr_view.next_unassigned_rank) {
            next_unassigned_rank--;
        }
    }
    //Copy member information, excluding the members that have failed
    int m = 0;
    for(int n = 0; n < curr_view.num_members; n++) {
        //This is why leave_ranks needs to be a set
        if(leave_ranks.find(n) == leave_ranks.end()) {
            members[m] = curr_view.members[n];
            member_ips[m] = curr_view.member_ips[n];
            failed[m] = curr_view.failed[n];
            ++m;
        }
    }

    //Initialize my_rank in next_view
    int32_t my_new_rank = -1;
    node_id_t myID = curr_view.members[curr_view.my_rank];
    for(int i = 0; i < next_num_members; ++i) {
        if(members[i] == myID) {
            my_new_rank = i;
            break;
        }
    }
    return std::make_unique<View>(curr_view.vid + 1, members, member_ips, failed,
                                  joined, departed, my_new_rank, next_unassigned_rank);
}

} /* namespace derecho */
